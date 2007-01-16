/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *\
 * This file is part of Suneido - The Integrated Application Platform
 * see: http://www.suneido.com for more information.
 * 
 * Copyright (c) 2000 Suneido Software Corp. 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation - version 2. 
 *
 * This program is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License in the file COPYING
 * for more details. 
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA
\* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "builtinclass.h"
#include "suboolean.h"
#include "sustring.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <vector> // for Readline
#include "sufinalize.h"
#include "minmax.h"

class SuFile : public SuFinalize
	{
public:
	void init(char* filename, char* mode);
	virtual void out(Ostream& os)
		{ os << "File('" << filename << "', '" << mode << "')"; }
	void close();
	static Method<SuFile>* methods()
		{
		static Method<SuFile> methods[] =
			{
			Method<SuFile>("Close", &SuFile::Close),
			Method<SuFile>("Flush", &SuFile::Flush),
			Method<SuFile>("Read", &SuFile::Read),
			Method<SuFile>("Readline", &SuFile::Readline),
			Method<SuFile>("Seek", &SuFile::Seek),
			Method<SuFile>("Tell", &SuFile::Tell),
			Method<SuFile>("Write", &SuFile::Write),
			Method<SuFile>("Writeline", &SuFile::Writeline),
			Method<SuFile>("", 0)
			};
		return methods;
		}
	const char* type() const
		{ return "File"; }
private:
	Value Read(BuiltinArgs&);
	Value Readline(BuiltinArgs&);
	Value Write(BuiltinArgs&);
	Value Writeline(BuiltinArgs&);
	Value Seek(BuiltinArgs&);
	Value Tell(BuiltinArgs&);
	Value Flush(BuiltinArgs&);
	Value Close(BuiltinArgs&);

	void ckopen(char* action);
	virtual void finalize();

	char* filename;
	const char* mode;
	FILE* f;
	char* end_of_line;
	};

Value su_file()
	{
	static BuiltinClass<SuFile> suFileClass;
	return &suFileClass;
	}

template<>
void BuiltinClass<SuFile>::out(Ostream& os)
	{ os << "File /* builtin class */"; }

template<>
Value BuiltinClass<SuFile>::instantiate(BuiltinArgs& args)
	{
	args.usage("usage: new File(filename, mode = 'r'");
	char* filename = args.getstr("filename");
	char* mode = args.getstr("mode", "r");
	args.end();
	SuFile* f = new BuiltinInstance<SuFile>();
	f->init(filename, mode);
	return f;
	}

template<>
Value BuiltinClass<SuFile>::callclass(BuiltinArgs& args)
	{
	args.usage("usage: File(filename, mode = 'r', block = false");
	char* filename = args.getstr("filename");
	char* mode = args.getstr("mode", "r");
	Value block = args.getValue("block", SuFalse);
	args.end();

	SuFile* f = new BuiltinInstance<SuFile>();
	f->init(filename, mode);
	if (block == SuFalse)
		return f;
	try
		{
		Value* sp = proc->stack.getsp();
		proc->stack.push(f);
		Value result = block.call(block, CALL, 1, 0, 0, -1);
		proc->stack.setsp(sp);
		f->close();
		return result;
		}
	catch(...)
		{
		f->close();
		throw ;
		}
	}

void SuFile::init(char* fn, char* m)
	{
	filename = fn;
	mode = m;
#ifdef _WIN32
	_fmode = O_BINARY; // set default mode
#endif

	end_of_line = (char*) (strchr(mode, 't') ? "\n" : "\r\n");

	if (*filename == 0 || ! (f = fopen(filename, mode)))
		except("File: can't open '" << filename << "' in mode '" << mode << "'");
	}

Value SuFile::Read(BuiltinArgs& args)
	{
	args.usage("usage: file.Read(nbytes = all)");
	int n = args.getint("nbytes", INT_MAX);
	args.end();

	ckopen("Read");
	if (feof(f))
		return SuFalse;
	int pos = ftell(f);
	fseek(f, 0, SEEK_END);
	int end = ftell(f);
	fseek(f, pos, SEEK_SET);
	n = min(n, end - pos);
	if (n <= 0)
		return SuFalse;
	SuString* s = new SuString(n);
	if (n != fread(s->buf(), 1, n, f))
		except("File: Read: error reading from: " << filename);
	return s;
	}

Value SuFile::Readline(BuiltinArgs& args)
	{
	args.usage("usage: file.Readline()");
	args.end();

	ckopen("Readline");
	int c;
	std::vector<char> buf;
	while (EOF != (c = fgetc(f)))
		{
		if (c == '\n')
			break ;
		if (c == '\r')
			{
			c = fgetc(f);
			if (c != EOF && c != '\n')
				ungetc(c, f);
			break ;
			}
		buf.push_back(c);
		}
	if (feof(f) && buf.size() == 0)
		return SuFalse;
	buf.push_back(0); // allow space for nul
	return new SuString(buf.size() - 1, &buf[0]); // no alloc
	}

Value SuFile::Write(BuiltinArgs& args)
	{
	args.usage("usage: file.Write(string)");
	Value arg = args.getValue("string");
	gcstring s = arg.gcstr();
	args.end();

	ckopen("Write");
	if (s.size() != fwrite(s.buf(), 1, s.size(), f))
		except("File: Write: error writing to: " << filename);
	return arg;
	}

Value SuFile::Writeline(BuiltinArgs& args)
	{
	args.usage("usage: file.Writeline(string)");
	Value arg = args.getValue("string");
	gcstring s = arg.gcstr();
	args.end();

	ckopen("Writeline");
	if (s.size() != fwrite(s.buf(), 1, s.size(), f) ||
		fputs(end_of_line, f) < 0)
		except("File: Write: error writing to: " << filename);
	return arg;
	}

Value SuFile::Seek(BuiltinArgs& args)
	{
	args.usage("usage: file.Seek(offset, origin)");
	int offset = args.getint("offset");
	gcstring origin_s = args.getgcstr("origin", "set");
	args.end();

	int origin;
	if (origin_s == "set")
		origin = SEEK_SET;
	else if (origin_s == "end")
		origin = SEEK_END;
	else if (origin_s == "cur")
		origin = SEEK_CUR;
	else
		except("file.Seek: origin must be 'set', 'end', or 'cur'");

	ckopen("Seek");
	return fseek(f, offset, origin) == 0 ? SuTrue : SuFalse;
	}

Value SuFile::Tell(BuiltinArgs& args)
	{
	args.usage("usage: file.Tell()");
	args.end();

	ckopen("Tell");
	return ftell(f);
	}

Value SuFile::Flush(BuiltinArgs& args)
	{
	args.usage("usage: file.Flush()");
	args.end();

	ckopen("Flush");
	fflush(f);
	return Value();
	}

Value SuFile::Close(BuiltinArgs& args)
	{
	args.usage("usage: file.Close()");
	args.end();

	ckopen("Close");
	close();
	return Value();
	}

void SuFile::ckopen(char* action)
	{
	if (! f)
		except("File: can't " << action << " a closed file: " << filename);
	}

void SuFile::close()
	{
	removefinal();
	finalize();
	}

void SuFile::finalize()
	{
	if (f)
		fclose(f);
	f = 0;
	}