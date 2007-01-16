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

#include "dump.h"
#include "suvalue.h"
#include "database.h"
#include "thedb.h"
#include "ostreamfile.h"
#include "ostreamstr.h"

static int dump1(OstreamFile& fout, const gcstring& table, bool output_name = true);

void dump(const gcstring& table)
	{
	if (table != "")
		{
		OstreamFile fout((table + ".su").str(), "wb");
		if (! fout)
			except("can't create " << table + ".su");
		fout << "Suneido dump 1.0" << endl;
		dump1(fout, table, false);
		}
	else
		{
		OstreamFile fout("database.su", "wb");
		if (! fout)
			except("can't create database.su");
		fout << "Suneido dump 1.0" << endl;
		for (Index::iterator iter = theDB()->get_index("tables", "tablename")->begin(schema_tran);
			! iter.eof(); ++iter)
			{
			Record r(iter.data());
			gcstring table = r.getstr(T_TABLE);
			if (theDB()->is_system_table(table))
				continue ;
			dump1(fout, table);
			}
		dump1(fout, "views");
		}
	}

static int dump1(OstreamFile& fout, const gcstring& table, bool output_name)
	{
	fout << "====== "; // load needs this same length as "create"
	if (output_name)
		fout << table << " ";
	theDB()->schema_out(fout, table);
	fout << endl;

	Lisp<gcstring> fields = theDB()->get_fields(table);
	static gcstring deleted = "-";
	bool squeeze = member(fields, deleted);

	int nrecs = 0;
	Index* idx = theDB()->first_index(table);
	verify(idx);
	int tran = theDB()->transaction(READONLY);
	for (Index::iterator iter = idx->begin(tran);
		! iter.eof(); ++iter, ++nrecs)
		{
		Record rec(iter.data());
		if (squeeze)
			{
			Record newrec(rec.cursize());
			int i = 0;
			for (Lisp<gcstring> f = fields; ! nil(f); ++f, ++i)
				if (*f != "-")
					newrec.addraw(rec.getraw(i));
			rec = newrec.dup();
			}
		int n = rec.cursize();
		fout.write(&n, sizeof n);
		fout.write(rec.ptr(), n);
		}
	theDB()->commit(tran);
	int zero = 0;
	fout.write(&zero, sizeof zero);
	return nrecs;
	}