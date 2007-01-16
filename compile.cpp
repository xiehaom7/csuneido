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

// NOTE: because of the static's this code is NOT re-entrant

#include "compile.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "std.h"
#include <vector>
#include "value.h"
#include "sunumber.h"
#include "scanner.h"
#include "sustring.h"
#include "suboolean.h"
#include "suobject.h"
#include "sufunction.h"
#include "type.h"
#include "structure.h"
#include "dll.h"
#include "callback.h"
#include "suclass.h"
#include "interp.h"
#include "globals.h"
#include "sudate.h"
#include "symbols.h"
#include "catstr.h"
#include "params.h"
#include "surecord.h"
#include "gc.h"

using namespace std;

template <class T> T* dup(const vector<T>& x)
	{
	T* y = new T[x.size()];
	uninitialized_copy(x.begin(), x.end(), y);
	return y;
	}

template <class T> T* dup(const vector<T>& x, NoPtrs)
	{
	T* y = new(noptrs) T[x.size()];
	uninitialized_copy(x.begin(), x.end(), y);
	return y;
	}

class Compiler
	{
public:
	explicit Compiler(char* s);
	Compiler(Scanner& sc, int t, int sn) // for FunctionCompiler
		: scanner(sc), stmtnest(sn), token(t)
		{ }
	Value constant(char* gname = 0);
	Value object();
	Value suclass(char* gname = 0);
	Value functionCompiler(short base = -1, bool newfn = false, char* gname = 0);
	Value dll();
	Value structure();
	Value callback();
	Value number();

	Scanner& scanner;
	int stmtnest; // if 0 then newline = end of statement
	int token;

	void match();
	bool binopnext();
	void match(int t);
	void match1();
	void matchnew();
	void matchnew(int t);
	void ckmatch(int t);
	NORETURN(syntax_error(char* err = ""));

	void member(SuObject* ob, char* gname = 0, short base = -1);
	ushort memname(char* gname, char* s);
	Params* params();
	char* ckglobal(char*);
private:
	bool valid_dll_arg_type();
	};

static ushort NEWNUM; // set by Compiler::Compiler

struct PrevLit
	{
	PrevLit()
		{ }
	PrevLit(int a, Value l, int i = -99) : adr(a), il(i), lit(l)
		{ }
	ushort adr;
	short il;
	Value lit;
	};

class FunctionCompiler : public Compiler
	{
public:
	FunctionCompiler(Scanner& scanner, int token, int stmtnest,
		short b, bool nf, char* gn = "")
		: Compiler(scanner, token, stmtnest),
		fn(0), last_adr(-1), nparams(0), ndefaults(0), 
		rest(false), newfn(nf), base(b), gname(gn), inblock(false),
		expecting_compound(false)
		{
		code.reserve(2000);
		db.reserve(500);
		}
	SuFunction* function();
private:
	SuFunction* fn;	// so local functions/classes can set parent
	vector<uchar> code;
	vector<Debug> db;
	short last_adr;
	vector<PrevLit> prevlits; // for emit const expr optimization
	vector<Value> literals;
	vector<ushort> locals;
	short nparams;
	short ndefaults;
	bool rest;
	bool newfn;
	short base;
	char* gname;
	bool inblock;
	bool expecting_compound;
	// for loops
	enum { maxtest = 100 };
	uchar test[maxtest];

	void block();
	void statement(short = -1, short* = NULL);
	void compound(short = -1, short* = NULL);
	void body();
	void stmtexpr();
	void exprlist();
	void opt_paren_expr();
	void expr()
		{ triop(); }
	void triop();
	void orop();
	void andop();
	void bitorop();
	void bitxor();
	void bitandop();
	void isop();
	void cmpop();
	void shift();
	void addop();
	void mulop();
	void unop();
	void expr0(bool newtype = false);
	void args(short&, vector<ushort>&, char* delims = "()");
	void record();
	short literal(Value);
	short emit_literal();
	short local();
	PrevLit poplits();
	short emit(short, short = 0, short = 0, short = 0, vector<ushort>* = 0);
	void patch(short);
	void mark();
	};

Value compile(char* s, char* gname)
	{
	Compiler compiler(s);
	Value x = compiler.constant(gname);
	if (compiler.token != -1)
		compiler.syntax_error();
	return x;
	}

Params* compile_params(char* s)
	{
	Compiler compiler(s);
	Params* params = compiler.params();
	if (compiler.token != -1)
		compiler.syntax_error();
	return params;
	}

// Compiler ---------------------------------------------------------------

Compiler::Compiler(char* s) : scanner(*new Scanner(strdup(s))), stmtnest(99)
	{
	NEWNUM = symnum("New");
	match(); // get first token
	}

Value Compiler::constant(char* gname)
	{
	Value x;
	switch (token)
		{
	case I_SUB :
		match();
		return -constant();
	case T_NUMBER :
		return number();
	case T_STRING :
		if (scanner.len == 0)
			x = SuString::empty_string;
		else
			x = new SuString(scanner.value, scanner.len);
		match();
		return x;
	case '#' :
		match();
		if (token == T_NUMBER)
			{
			if (! (x = SuDate::literal(scanner.value)))
				syntax_error("bad date literal");
			match();
			return x;
			}
		else if (token == T_IDENTIFIER || token == T_STRING ||
			token == T_AND || token == T_OR || token == I_NOT ||
			token == I_IS || token == I_ISNT)
			{
			x = symbol(scanner.value);
			match();
			return x;
			}
		else if (token != '(' && token != '{' && token != '[')
			syntax_error("invalid literal following '#'");
		// else fall thru
	case '(' :
	case '{' :
	case '[' :
		return object();
	case T_IDENTIFIER :
		switch (scanner.keyword)
			{
		case K_FUNCTION :
			matchnew();
			return functionCompiler();
		case K_CLASS :
			return suclass(gname);
		case K_DLL :
			return dll();
		case K_STRUCT :
			return structure();
		case K_CALLBACK :
			return callback();
		case K_TRUE :
			match(); 
			return SuBoolean::t; 
		case K_FALSE :
			match(); 
			return SuBoolean::f; 
		default :
			if (*scanner.peek() == '{')
				return suclass(gname);
			// else identifier => string
			x = new SuString(scanner.value);
			match();
			return x;
			}
		}
	syntax_error();
	return Value();
	}

Value Compiler::number()
	{
	Value result = SuNumber::literal(scanner.value);
	match(T_NUMBER);
	return result;
	}

Value Compiler::object() //=======================================
	{
	SuObject* ob = 0;
	char end = 0;
	if (token == '(')
		{
		ob = new SuObject();
		end = ')';
		match();
		}
	else if (token == '{' || token == '[')
		{
		ob = new SuRecord();
		end = token == '{' ? '}' : ']';
		match();
		}
	else
		syntax_error();
	while (token != end)
		{
		member(ob);
		if (token == ',' || token == ';')
			match();
		}
	match(end);
	ob->set_readonly();
	return ob;
	}

char* Compiler::ckglobal(char* s)
	{
	if (! (isupper(*s) || (*s == '_' && isupper(s[1]))))
		syntax_error("base class must be global defined in library");
	return s;
	}

Value Compiler::suclass(char* gname) //===========================
	{
	if (! gname)
		{
		static int classnum = 0;
		char buf[32] = "Class";
		itoa(classnum++, buf + 5, 10);
		gname = buf;
		}

	short base = OBJECT;
	if (scanner.keyword == K_CLASS)
		{
		matchnew();
		if (token == ':')
			{
			matchnew();
			if (*scanner.value == '_')
				base = globals.copy(ckglobal(scanner.value + 1));
			else
				base = globals(ckglobal(scanner.value));
			matchnew(T_IDENTIFIER);
			}
		}
	else
		{
		if (*scanner.value == '_')
			base = globals.copy(ckglobal(scanner.value + 1));
		else
			base = globals(ckglobal(scanner.value));
		matchnew(T_IDENTIFIER);
		}
	SuClass *ob = new SuClass(base);
	match('{');
	while (token != '}')
		{
		member(ob, gname, base);
		if (token == ',' || token == ';')
			match();
		}
	match('}');
	ob->set_readonly();
	return ob;
	}

// object constant & class members
void Compiler::member(SuObject* ob, char* gname, short base)
	{
	Value mv;
	int mi = -1;
	bool minus = false;
	if (token == I_SUB)
		{
		minus = true;
		match();
		if (token != T_NUMBER)
			syntax_error();
		}
	bool default_allowed = true;
	char peek = *scanner.peek();
	if (peek == ':' || (base > 0 && peek == '('))
		{
		if (token == T_IDENTIFIER || token == T_STRING)
			{
			mv = symbol(mi = memname(gname, scanner.value));
			match();
			}
		else if (token == T_NUMBER)
			{
			mv = number();
			if (minus)
				{
				mv = -mv;
				minus = false;
				}
			}
		else
			syntax_error();
		if (token == ':')
			match();
		}
	else
		default_allowed = false;

	Value x;
	if (peek == '(' && base > 0)
		x = functionCompiler(base, mi == NEWNUM, gname);
	else if (token != ',' && token != ')' && token != '}')
		{
		x = constant();
		if (minus)
			x = -x;
		}
	else if (default_allowed)
		x = SuBoolean::t; // default value
	else
		syntax_error();

	if (mv)
		{
		if (ob->get(mv))
			except("duplicate member name (" << mv << ")");
		ob->put(mv, x);
		}
	else
		ob->add(x);
	if (mi != -1)
		if (Named* nx = x.get_named())
			if (Named* nob = ob->get_named())
				{
				nx->parent = nob;
				nx->num = mi;
				}
	}

// struct, dll, callback -------------------------------------------------

const int maxitems = 100;

Value Compiler::structure()
	{
	matchnew();
	match('{');
	TypeItem memtypes[maxitems];
	ushort memnames[maxitems];
	short n = 0;
	for (; token == T_IDENTIFIER; ++n)
		{
		except_if(n >= maxitems, "too many structure members");
		memtypes[n].gnum = globals(scanner.value);
		match();
		memtypes[n].n = 1;
		// NOTE: don't allow pointer & array in same type
		if (token == I_MUL)
			{ // pointer
			match();
	   		memtypes[n].n = 0;
			}
		else if (token == '[')
			{ // array
			match('[');
			memtypes[n].n = -atoi(scanner.value);
			match(T_NUMBER);
			match(']');
			}
		memnames[n] = symnum(scanner.value);
	   	match(T_IDENTIFIER);
		if (token == ';')
			match();
		}
	match('}');
	return new Structure(memtypes, memnames, n);
	}

Value Compiler::dll()
	{
	matchnew();
	short rtype;
	switch (scanner.keyword)
		{
	case K_VOID :
		rtype = 0;
		break ;
	case K_BOOL :
	case K_CHAR : case K_SHORT : case K_LONG : case K_INT64 :
	case K_FLOAT : case K_DOUBLE :
	case K_STRING :
	case K_HANDLE : case K_GDIOBJ :
		rtype = globals(scanner.value);
		break ;
	default :
		syntax_error("invalid dll return type");
		}
	matchnew(T_IDENTIFIER);

	const int maxname = 80;
	char library[maxname];
	strncpy(library, scanner.value, maxname);
	matchnew(T_IDENTIFIER);
	matchnew(':');
	char name[maxname];
	strncpy(name, scanner.value, maxname);
	matchnew(T_IDENTIFIER);
	if (token == '@')
		{
		if (strlen(name) + 1 < maxname)
			strcat(name, "@");
		matchnew('@');
		if (strlen(name) + strlen(scanner.value) < maxname)
			strcat(name, scanner.value);
		matchnew(T_NUMBER);
		}

	match('(');
	TypeItem paramtypes[maxitems];
	ushort paramnames[maxitems];
	int n = 0;
	for (; token != ')'; ++n)
		{
		except_if(n >= maxitems, "too many dll parameters");
		if (token == '[')
			{
			match('[');
			match(K_IN);
			match(']');
			if (scanner.keyword != K_STRING)
				syntax_error();
			paramtypes[n].gnum = globals("instring");
			}
		else if (token == T_IDENTIFIER)
			{
			if (! valid_dll_arg_type())
				syntax_error("invalid dll parameter type");
			paramtypes[n].gnum = globals(scanner.value);
			}
		else
			syntax_error();
		match();
		paramtypes[n].n = 1;
		if (token == I_MUL)
			{ // pointer
			match();
	   		paramtypes[n].n = 0;
			}
		paramnames[n] = symnum(scanner.value);
	   	match(T_IDENTIFIER);
		if (token == ',')
			match();
		}
	match(')');
	return new Dll(rtype, library, name, paramtypes, paramnames, n);
	}

bool Compiler::valid_dll_arg_type()
	{
	if (isupper(*scanner.value))
		return true;
	switch (scanner.keyword)
		{
	case K_BOOL : case K_FLOAT : case K_DOUBLE :
	case K_CHAR : case K_SHORT : case K_LONG : case K_INT64 :
	case K_STRING : case K_BUFFER :
	case K_HANDLE : case K_GDIOBJ : case K_RESOURCE :
		return true;
		}
	return false;
	}

Value Compiler::callback()
	{
	matchnew();
	match('(');
	TypeItem paramtypes[maxitems];
	ushort paramnames[maxitems];
	short n = 0;
	for (; token == T_IDENTIFIER; ++n)
		{
		except_if(n >= maxitems, "too many callback parameters");
		paramtypes[n].gnum = globals(scanner.value);
		match();
		paramtypes[n].n = 1;
		if (token == I_MUL)
			{ // pointer
			match();
	   		paramtypes[n].n = 0;
			}
		paramnames[n] = symnum(scanner.value);
	   	match(T_IDENTIFIER);
		if (token == ',')
			match();
		}
	match(')');
	return new Callback(paramtypes, paramnames, n);
	}

// scanner functions ------------------------------------------------

void Compiler::match(int t)
	{
	ckmatch(t);
	match();
	}

void Compiler::match()
	{
	match1();
	if (stmtnest != 0 || binopnext())
		while (token == T_NEWLINE)
			match1();
	}

bool Compiler::binopnext()
	{
	char* s = scanner.peek();
	switch (*s)
		{
	case '?' :
	case '*' :
	case '%' :
	case '&' :
	case '|' :
	case '<' :
	case '>' :
	case '=' :
	case '!' :
	case '^' :
	case '$' :
		return true;
	case '/' :
		if (s[1] == '*')
			return false;
		// fall thru
	case '+' :
	case '-' :
		return s[1] != s[0]; // but not //, ++, --
	default :
		return false;
		}
	}

void Compiler::matchnew(int t)
	{
	ckmatch(t);
	matchnew();
	}

void Compiler::matchnew()
	{
	do
		match1();
		while (token == T_NEWLINE);
	}

void Compiler::match1()
	{
	if (token == '{' || token == '(' || token == '[')
		++stmtnest;
	if (token == '}' || token == ')' || token == ']')
		--stmtnest;
	token = scanner.next();
	}	

void Compiler::ckmatch(int t)
	{
	if (t != (t < KEYWORDS ? token : scanner.keyword))
		syntax_error();
	}

void Compiler::syntax_error(char* err)
	{
	// figure out the line number
	int line = 1;
	for (int i = 0; i < scanner.prev; ++i)
		if (scanner.source[i] == '\n')
			++line;

	if (! *err && token == T_ERROR)
		err = scanner.err;
	except("syntax error at line " << line << "  " << err);
	}

Params* Compiler::params()
	{
	bool rest = false;
	vector<ushort> pnames;
	vector<Value> defaults;
	for (;;)
		{
		if (token == '@')
			{
			match();
			pnames.push_back(symnum(scanner.value));
			match();
			rest = true;
			break ;
			}
		else if (token != T_IDENTIFIER)
			break ;

		pnames.push_back(symnum(scanner.value));
		match();
		if (token == I_EQ)
			{
			match();
			defaults.push_back(constant());
			}
		else if (defaults.size() > 0)
			syntax_error("default parameters must come last");
		if (token == ',')
			match();
		}
	return new Params(scanner.source, pnames.size(), defaults.size(), rest, 
		pnames.empty() ? 0 : &pnames[0], defaults.empty() ? 0 : &defaults[0]);
	}

// function ---------------------------------------------------------

Value Compiler::functionCompiler(short base, bool newfn, char* gname)
	{
	FunctionCompiler compiler(scanner, token, stmtnest, base, newfn, gname);
	Value fn = compiler.function();
	token = compiler.token;
	return fn;
	}

SuFunction* FunctionCompiler::function()
	{
	fn = new SuFunction;  // need this while code is generated

	// parameters
	match('(');
	if (token == '@')
		{
		match();
		local();
		match(T_IDENTIFIER);
		rest = true;
		nparams = 1;
		ndefaults = 0;
		}
	else
		{
		rest = false;
		for (nparams = ndefaults = 0; token != ')'; ++nparams)
			{
			int i = local();
			if (i != locals.size() - 1)
				except("duplicate function parameter (" << scanner.value << ")");
			match(T_IDENTIFIER);

			if (token == I_EQ)
				{
				match();
				verify(ndefaults == literal(constant()));
				++ndefaults;
				}
			else if (ndefaults)
				syntax_error("default parameters must come last");
			if (token != ')')
				match(',');
			}
		}
	matchnew(')');

	if (token != '{')
		syntax_error();
	body();

	mark();

	if (code.size() > SHRT_MAX)
		except("can't compile code larger than 32K");
	fn->code = dup(code, noptrs);
	fn->nc = code.size();
	fn->db = dup(db, noptrs);
	fn->nd = db.size();
	fn->literals = dup(literals);
	fn->nliterals = literals.size();
	fn->locals = dup(locals, noptrs);
	fn->nlocals = locals.size();
	fn->nparams = nparams;
	fn->ndefaults = ndefaults;
	fn->rest = rest;
	fn->src = scanner.source; // NOTE: all functions within it share the same source string
	return fn;
	}

void FunctionCompiler::body()
	{
	compound(); // NOTE: caller must check for {
	int last = last_adr > 0 ? code[last_adr] : -1;
	if (last == I_POP)
		code[last_adr] = I_RETURN;
	else if (I_EQ_AUTO <= last && last < I_PUSH && (last & 8))
		{
		code[last_adr] &= ~8;	// clear POP
		emit(I_RETURN);
		}
	else if (last != I_RETURN && last != I_RETURN_NIL)
		{
		mark();
		emit(I_RETURN_NIL);
		}
	}

void FunctionCompiler::block()
	{
	int first = locals.size();
	if (*scanner.peek() == '|')
		{ // parameters
		match('{');
		match(I_BITOR);
		while (token == T_IDENTIFIER)
			{
			locals.push_back(symnum(scanner.value)); // ensure new
			match();
			if (token == ',')
				match();
			}
		if (token != I_BITOR) // i.e. |
			syntax_error();
		}
	int a = emit(I_BLOCK, 0, -1);
	code.push_back(first);
	int nparams = locals.size() - first;
	code.push_back(nparams); // number of params
	bool prev_inblock = inblock;
	inblock = true; // for break & continue
	body();
	inblock = prev_inblock;
	patch(a);
	// hide block parameter locals from rest of code
	for (int i = 0; i < nparams; ++i)
		locals[first + i] = symnum(CATSTRA("_", symstr(locals[first + i])));
	}

void FunctionCompiler::compound(short cont, short* pbrk)
	{
	match(); // NOTE: caller must check for {
	if (newfn && last_adr == -1)
		{
		if (scanner.keyword != K_SUPER || *scanner.peek() != '(')
			{
			emit(I_SUPER, 0, base);
			emit(I_CALL, MEM_SELF, NEWNUM);
			emit(I_POP);
			}
		}
	while (token != '}')
		statement(cont, pbrk);
	match('}');
	}

#define OPT_PAREN_EXPR1 \
	bool parens = (token == '('); \
	int prev_stmtnest = stmtnest; \
	if (parens) \
		match('(');

#define OPT_PAREN_EXPR2 \
	if (! parens) \
		{ stmtnest = 0; /* enable newlines */ expecting_compound = true; } \
	expr(); \
	if (parens) \
		match(')'); \
	else if (token == T_NEWLINE) \
		match(); \
	stmtnest = prev_stmtnest; \
	expecting_compound = false;

void FunctionCompiler::opt_paren_expr()
	{
	OPT_PAREN_EXPR1
	OPT_PAREN_EXPR2
	}

void FunctionCompiler::statement(short cont, short* pbrk)
	{
	short a, b, c;

	a = code.size();	// before mark to include nop
	mark();

	switch (scanner.keyword)
		{
	case K_IF :
		b = -1;
		for (;;)
			{
			match();
			opt_paren_expr();
			a = emit(I_JUMP, POP_NO, -1);
			statement(cont, pbrk);
			if (scanner.keyword == K_ELSE)
				{
				b = emit(I_JUMP, UNCOND, b);
				patch(a);
				mark();
				match();
				if (scanner.keyword == K_IF)
					continue ;
				statement(cont, pbrk);
				}
			else
				patch(a);
			break ;
			}
		patch(b);
		break ;
	case K_FOREVER :
		b = -1;
		match();
		statement(a, &b);
		emit(I_JUMP, UNCOND, a - (code.size() + 3));
		patch(b);
		break ;
	case K_WHILE :
		match();
		opt_paren_expr();
		b = emit(I_JUMP, POP_NO, -1);
		statement(a, &b);
		emit(I_JUMP, UNCOND, a - (code.size() + 3));
		patch(b);
		break ;
	case K_DO :
		{
		b = -1;
		match();
		short skip = emit(I_JUMP, UNCOND, -1);
		short c = emit(I_JUMP, UNCOND, -1);
		patch(skip);
		short body = code.size();
		statement(c, &b);
		patch(c);
		mark();
		match(K_WHILE);
		opt_paren_expr();
		if (token == ';' || token == T_NEWLINE)
			match();
		emit(I_JUMP, POP_YES, body - (code.size() + 3));
		patch(b);
		break ;
		}
	case K_FOREACH :
		{
		static ushort ITERKEYS = symnum("IterKeys");
		static ushort ITERLIST = symnum("IterList");
		static ushort ITERVALUES = symnum("Iter");
		static ushort ITERLISTVALUES = symnum("IterListValues");
		static ushort NEXT = symnum("Next");

		match();
		OPT_PAREN_EXPR1
		bool list = false;
		if (scanner.keyword == K_LIST)
			{
			list = true;
			match();
			}
		bool value = false;
		if (scanner.keyword == K_VALUE)
			{
			value = true;
			match();
			}
		short var = local();
		match(T_IDENTIFIER);
		match(K_IN);
		OPT_PAREN_EXPR2
		emit(I_CALL, MEM, (value 
			? (list ? ITERLISTVALUES : ITERVALUES)
			: (list ? ITERLIST : ITERKEYS)));
		a = code.size();
		emit(I_DUP); // to save the iterator
		emit(I_DUP); // to compare against for end
		emit(I_CALL, MEM, NEXT);
		emit(I_EQ, 0x80 + (AUTO << 4), var);
		emit(I_ISNT);
		b = emit(I_JUMP, POP_NO, -1);
		statement(a, &b);
		emit(I_JUMP, UNCOND, a - (code.size() + 3));
		patch(b);
		emit(I_POP);
		last_adr = -1; // prevent POP from being optimized away
		break ;
		}
	case K_FOR :
		{
		b = -1;
		match();
		OPT_PAREN_EXPR1
		if (token != ';')
			{
			// INITIALIZATION
			int nc_before = code.size();
			exprlist();
			if (scanner.keyword == K_IN)
				{ // for (var in expr)
				match();
				int codelen = code.size() - nc_before;
				int var = 0;
				if (codelen == 1 && (code[nc_before] & 0xf0) == I_PUSH_AUTO)
					var = code[nc_before] & 15;
				else if (codelen == 2 && code[nc_before] == (I_PUSH | AUTO))
					var = code[nc_before + 1];
				else
					syntax_error("usage: for (local_variable in expression)");
				code.resize(nc_before);
				last_adr = -1;
				OPT_PAREN_EXPR2
				static ushort ITER = symnum("Iter");
				emit(I_CALL, MEM, ITER);
				a = code.size();
				emit(I_DUP); // to save the iterator
				emit(I_DUP); // to compare against for end
				static ushort NEXT = symnum("Next");
				emit(I_CALL, MEM, NEXT);
				emit(I_EQ, 0x80 + (AUTO << 4), var);
				emit(I_ISNT);
				b = emit(I_JUMP, POP_NO, -1);
				statement(a, &b);
				emit(I_JUMP, UNCOND, a - (code.size() + 3));
				patch(b);
				emit(I_POP);
				last_adr = -1; // prevent POP from being optimized away
				break ;
				}
			emit(I_POP);
			}
		if (! parens)
			syntax_error("usage: for (expr; expr; expr)");
		match(';');
		short aa = code.size();
		short test_nc = 0;
		if (token != ';')
			{
			// TEST
			test_nc = code.size();
			expr();
			// cut out the test code
			if (code.size() - test_nc >= maxtest)
				syntax_error("for loop test too large");
			copy(code.begin() + test_nc, code.end(), test);
			int tmp = code.size();
			code.erase(code.begin() + test_nc, code.end());
			test_nc = tmp - test_nc;
			}
		match(';');
		if (token != ')')
			{
			// INCREMENT
			c = emit(I_JUMP, UNCOND, -1);	// skip over the increment
			aa = code.size();
			exprlist();
			emit(I_POP);
			patch(c);
			}
		match(')');
		// paste the test code back in after the increment
		if (test_nc)
			{
			code.insert(code.end(), test, test + test_nc);
			b = emit(I_JUMP, POP_NO, b);
			}
		// BODY
		if (a != code.size() - 1)
			a = aa;
		statement(a, &b);
		emit(I_JUMP, UNCOND, a - (code.size() + 3));
		patch(b);
		break ;
		}
	case K_CONTINUE :
		match();
		if (token == ';')
			match();
		if (cont >= 0)
			emit(I_JUMP, UNCOND, cont - (code.size() + 3));
		else if (inblock)
			{
			static Value con("block:continue");
			emit(I_PUSH, LITERAL, literal(con));
			emit(I_THROW);
			}
		else
			syntax_error();
		break ;
	case K_BREAK :
		match();
		if (token == ';')
			match();
		if (pbrk)
			*pbrk = emit(I_JUMP, UNCOND, *pbrk);
		else if (inblock)
			{
			static Value brk("block:break");
			emit(I_PUSH, LITERAL, literal(brk));
			emit(I_THROW);
			}
		else
			syntax_error();
		break ;
	case K_RETURN :
		match1(); // don't discard newline
		if (inblock)
			{
			switch (token)
				{
			case ';' :
			case T_NEWLINE :
				match();
				// fall thru
			case '}' :
				emit(I_PUSH, LITERAL, literal(Value()));
				break ;
			default :
				stmtexpr();
				}
			static Value ret("block return");
			emit(I_PUSH, LITERAL, literal(ret));
			emit(I_THROW);
			}
		else
			switch (token)
				{
			case ';' :
			case T_NEWLINE :
				match();
				// fall thru
			case '}' :
				emit(I_RETURN_NIL);
				break ;
			default :
				stmtexpr();
				emit(I_RETURN);
				}
		break ;
	case K_SWITCH :
		a = -1;
		match();
		opt_paren_expr();
		match('{');
		while (scanner.keyword == K_CASE)
			{
			mark();
			match();
			b = -1;
			for (;;)
				{
				expr();
				if (token == ',')
					{
					b = emit(I_JUMP, CASE_YES, b);
					match();
					}
				else
					{
					c = emit(I_JUMP, CASE_NO, -1);
					break ;
					}
				}
			match(':');
			patch(b);
			while (scanner.keyword != K_CASE && 
				scanner.keyword != K_DEFAULT && token != '}')
				statement(cont, pbrk);
			a = emit(I_JUMP, UNCOND, a);
			patch(c);
			}
		if (scanner.keyword == K_DEFAULT)
			{
			mark();
			match();
			match(':');
			emit(I_POP);
			while (token != '}')
				statement(cont, pbrk);
			a = emit(I_JUMP, UNCOND, a);
			}
		match('}');
		emit(I_POP);
		patch(a);
		break ;
	case K_TRY :
		{
		match();
		a = emit(I_TRY, 0, -1);
		uchar catchvalue = literal(0);
		code.push_back(catchvalue);
		statement(cont, pbrk);	// try code
		mark();
		b = emit(I_CATCH, 0, -1);
		patch(a);
		Value value = SuString::empty_string;
		if (scanner.keyword == K_CATCH)
			{
			match();
			if (token == '(')
				{
				match('(');
				if (token != ')')
					{
					ushort exception = local();
					match(T_IDENTIFIER);
					if (token == ',')
						{
						match();
						if (token == T_STRING)
							value = new SuString(scanner.value, scanner.len);
						match(T_STRING);
						}
					emit(I_EQ, 0x80 + (AUTO << 4), exception);
					}
				match(')');
				}
			emit(I_POP);
			statement(cont, pbrk);	// catch code
			}
		else
			{
			emit(I_POP);
			}
		literals[catchvalue] = value;
		patch(b);
		break ;
		}
	case K_THROW :
		match();
		stmtexpr();
		emit(I_THROW);
		break ;
	default :
		if (token == ';')
			match();
		else if (token == '{')
			compound(cont, pbrk);
		else
			{
			stmtexpr();
			emit(I_POP);
			}
		}
	}

void FunctionCompiler::stmtexpr()
	{
	int prev_stmtnest = stmtnest;
	stmtnest = 0; // enable newlines
	expr();
	stmtnest = prev_stmtnest;
	if (token == ';' || token == T_NEWLINE)
		match();
	else if (token != '}' &&
		scanner.keyword != K_CATCH &&
		scanner.keyword != K_WHILE)
		syntax_error();
	}

void FunctionCompiler::exprlist() // used by for
	{
	expr();
	while (token == ',')
		{
		match();
		emit(I_POP);
		expr();
		}
	}

void FunctionCompiler::triop()
	{
	orop();
	if (token == '?')
		{
		++stmtnest;
		match();
		short a = emit(I_JUMP, POP_NO, -1);
		triop();
		match(':');
		--stmtnest;
		short b = emit(I_JUMP, UNCOND, -1);
		patch(a);
		triop();
		patch(b);
		}
	}

void FunctionCompiler::orop()
	{
	andop();
	short a = -1;
	while (token == T_OR)
		{
		matchnew();
		a = emit(I_JUMP, ELSE_POP_YES, a);
		andop();
		}
	patch(a);
	}

void FunctionCompiler::andop()
	{
	bitorop();
	short a = -1;
	while (token == T_AND)
		{
		matchnew();
		a = emit(I_JUMP, ELSE_POP_NO, a);
		bitorop();
		}
	patch(a);
	}

void FunctionCompiler::bitorop()
	{
	bitxor();
	while (token == I_BITOR)
		{
		short t = token;
		matchnew();
		bitxor();
		emit(t);
		}
	}

void FunctionCompiler::bitxor()
	{
	bitandop();
	while (token == I_BITXOR)
		{
		short t = token;
		matchnew();
		bitandop();
		emit(t);
		}
	}

void FunctionCompiler::bitandop()
	{
	isop();
	while (token == I_BITAND)
		{
		short t = token;
		matchnew();
		isop();
		emit(t);
		}
	}

void FunctionCompiler::isop()
	{
	cmpop();
	while (I_IS <= token && token <= I_MATCHNOT)
		{
		short t = token;
		matchnew();
		cmpop();
		emit(t);
		}
	}

void FunctionCompiler::cmpop()
	{
	shift();
	while (token == I_LT || token == I_LTE || token == I_GT || token == I_GTE)
		{
		short t = token;
		matchnew();
		shift();
		emit(t);
		}
	}

void FunctionCompiler::shift()
	{
	addop();
	while (token == I_LSHIFT || token == I_RSHIFT)
		{
		short t = token;
		matchnew();
		addop();
		emit(t);
		}
	}

void FunctionCompiler::addop()
	{
	mulop();
	while (token == I_ADD || token == I_SUB || token == I_CAT)
		{
		short t = token;
		matchnew();
		mulop();
		emit(t);
		}
	}

void FunctionCompiler::mulop()
	{
	unop();
	while (token == I_MUL || token == I_DIV || token == I_MOD)
		{
		short t = token;
		matchnew();
		unop();
		emit(t);
		}
	}

void FunctionCompiler::unop()
	{
	if (token == I_NOT || token == I_ADD || token == I_SUB || token == I_BITNOT)
		{
		short t = token;
		match();
		unop();
		if (t != I_ADD)	// should have I_UPLUS
			emit(t == I_SUB ? I_UMINUS : t);
		}
	else if (scanner.keyword == K_NEW)
		{
		static short INSTANTIATE = symnum("instantiate");

		match();
		expr0(true);
		short nargs = 0;
		vector<ushort> argnames;
		if (token == '(')
			args(nargs, argnames);
		emit(I_CALL, MEM, INSTANTIATE, nargs, &argnames);
		}
	else
		expr0();
	}

short FunctionCompiler::emit_literal()
	{
	return emit(I_PUSH, LITERAL, literal(constant()));
	}

void FunctionCompiler::expr0(bool newtype)
	{
	bool lvalue = true;
	bool value = true;
	short option = -1;
	short id = -1;
	short incdec = 0;
	if (token == I_PREINC || token == I_PREDEC)
		{
		incdec = token;
		matchnew();
		}
	bool super = false;
	switch (token)
		{
	case T_NUMBER :
	case T_STRING :
	case '#' :
		emit_literal();
		option = LITERAL;
		lvalue = value = false;
		break ;
	case '{' : // block 
		block();
		option = LITERAL;
		lvalue = value = false;
		break ;
	case T_IDENTIFIER :
		switch (scanner.keyword)
			{
		case K_FUNCTION :
		case K_CLASS :
		case K_DLL :
		case K_STRUCT :
		case K_CALLBACK :
			emit_literal();
			option = LITERAL;
			lvalue = value = false;
			break ;
		case K_SUPER :
			match();
			super = true;
			if (incdec || base < 0)
				syntax_error();
			if (last_adr == 0 // not -1 because mark() has generated NOP
				&& newfn && token == '(')
				{
				// only allow super(...) at start of new function
				option = MEM_SELF;
				id = NEWNUM;
				}
			else
				{
				match('.');
				option = MEM_SELF;
				id = symnum(scanner.value);
				match(T_IDENTIFIER);
				}
			break ;
		default :
			if (isupper(scanner.value[*scanner.value == '_' ? 1 : 0]) && 
				'{' == (expecting_compound ? scanner.peeknl() : *scanner.peek()))
				{ // Name { => class
				emit_literal();
				option = LITERAL;
				lvalue = value = false;
				}
			else
				{
				option = *scanner.value == '_'
					? isupper(scanner.value[1]) ? LITERAL : DYNAMIC
					: isupper(scanner.value[0]) ? GLOBAL : AUTO;
				if (option == GLOBAL)
		  			{
					lvalue = false;
					id = globals(scanner.value);
					if (id == TrueNum || id == FalseNum)
						{
						emit(I_PUSH, LITERAL, 
							literal(id == TrueNum ? SuTrue : SuFalse));
						lvalue = value = false;
						}
					}
				else if (option == LITERAL) // _Name
					{
					Value x = globals.get(scanner.value + 1);
					if (! x)
						except(scanner.value << " not found");
					emit(I_PUSH, LITERAL, literal(x));
					lvalue = value = false;
					}
				else // AUTO or DYNAMIC
		  			{
					switch (scanner.keyword)
						{
					case K_THIS :
						emit(I_PUSH_VALUE, SELF);
						lvalue = value = false;
						break ;
					case K_TRUE :
					case K_FALSE :
						emit(I_PUSH, LITERAL, 
							literal(scanner.keyword == K_TRUE ? SuTrue : SuFalse));
						lvalue = value = false;
						break ;
					default :
						id = local();
						}
					}
				match();
				}
			break ;
			}
		break ;
	case '.' :
		matchnew('.');
		option = MEM_SELF;
		id = memname(gname, scanner.value);
		match(T_IDENTIFIER);
		if (id == NEWNUM)
			syntax_error();
		break ;
	case '[' :
		record();
		option = LITERAL;
		lvalue = value = super = false;
		break ;
	case '(' :
		match('(');
		expr();
		match(')');
		option = LITERAL;
		lvalue = value = false;
		break ;
	default :
		syntax_error();
		}
	while (token == '.' || token == '[' || token == '(')
		{
		if (value && token != '(')
			{
			emit(I_PUSH, option, id);
			option = -1;
			}
		if (newtype && token == '(')
			{
			if (value)
				emit(I_PUSH, option, id);
			return ;
			}
		lvalue = value = true;
		if (token == '.')
			{
			matchnew();
			option = MEM;
			id = symnum(scanner.value);
			match(T_IDENTIFIER);
			if (id == NEWNUM)
				syntax_error();
			}
		else if (token == '[')
			{
			match('[');
			expr();
			option = SUB;
			match(']');
			}
		else if (token == '(')
			{
			short nargs;
			vector<ushort> argnames;
			args(nargs, argnames);
			if (super)
				emit(I_SUPER, 0, base);
			emit(I_CALL, option, id, nargs, &argnames);
			option = LITERAL;
			lvalue = value = super = false;
			}
		else
			break ;
		}
	if (incdec)
		{
		if (! lvalue)
			syntax_error();
		emit(incdec, 0x80 + (option << 4), id);
		}
	else if (I_ADDEQ <= token && token <= I_EQ)
		{
		short t = token;
		if (! lvalue)
			syntax_error();
		matchnew();
		if (scanner.keyword == K_FUNCTION || scanner.keyword == K_CLASS)
			{
			if (t != I_EQ)
				syntax_error();
			Value k = constant();
			Named* n = k.get_named();
			verify(n);
			n->parent = &fn->named;
			n->num = (option == AUTO || option == DYNAMIC ?
					locals[id] : id);
			emit(I_PUSH, LITERAL, literal(k));
			}
		else
			expr();
		emit(t, 0x80 + (option << 4), id);
		}
	else if (token == I_PREINC || token == I_PREDEC)
		{
		if (! lvalue)
			syntax_error();
		emit(token + 2, 0x80 + (option << 4), id);
		match();
		}
	else if (value)
		{
		emit(I_PUSH, option, id);
		}
	}

void FunctionCompiler::args(short& nargs, vector<ushort>& argnames, char* delims)
	{
	nargs = 0;
	match(delims[0]);
	if (token == '@')
		{
		match();
		short each = 0;
		if (token == I_ADD)
			{
			match();
			each = strtoul(scanner.value, NULL, 0);			
			match(T_NUMBER);
			}
		expr();
		emit(I_EACH, 0, each);
		nargs = 1;
		match(delims[1]);
		}
	else
		{
		bool key = false;
		for (nargs = 0; token != delims[1]; ++nargs)
			{
			if (*scanner.peek() == ':')
				{
				key = true;
				int id;
				if (token == T_IDENTIFIER || token == T_STRING)
					id = symnum(scanner.value);
				else if (token == T_NUMBER)
					{
					id = strtoul(scanner.value, NULL, 0);
					if (id >= 0x8000)
						except("numeric subscript overflow: (" << scanner.value << ")");
					}
				else
					syntax_error();
				if (find(argnames.begin(), argnames.end(), id) != argnames.end())
					except("duplicate argument name (" << scanner.value << ")");
				argnames.push_back(id);
				match();
				match();
				}
			else if (key)
				syntax_error("un-named arguments must come before named arguments");
			if (key && (*scanner.peek() == ':' || token == ',' || token == delims[1]))
				emit(I_PUSH, LITERAL, literal(SuTrue));
			else
				expr();
			if (token == ',')
				match();
			}
		match(delims[1]);
		if (token == T_NEWLINE && ! expecting_compound && *scanner.peek() == '{')
			match();
		if (token == '{')
			{ // take block following args as another arg
			static ushort n_block = symnum("block");
			argnames.push_back(n_block);
			block();
			++nargs;
			}
		}
	}

void FunctionCompiler::record()
	{
	SuRecord* rec = 0;
	vector<ushort> argnames;
	short nargs = 0;
	match('[');
	bool key = false;
	int argi = 0;
	for (nargs = 0; token != ']'; ++nargs, ++argi)
		{
		if (*scanner.peek() == ':')
			{
			key = true;
			int id;
			if (token == T_IDENTIFIER || token == T_STRING)
				id = symnum(scanner.value);
			else if (token == T_NUMBER)
				{
				id = strtoul(scanner.value, NULL, 0);
				if (id >= 0x8000)
					except("numeric subscript overflow: (" << scanner.value << ")");
				}
			else
				syntax_error();
			if (find(argnames.begin(), argnames.end(), id) != argnames.end())
				except("duplicate member name (" << scanner.value << ")");
			argnames.push_back(id);
			match();
			match();
			}
		else if (key)
			syntax_error();
		prevlits.clear();
		if (key && (*scanner.peek() == ':' || token == ',' || token == ']'))
			emit(I_PUSH, LITERAL, literal(SuTrue));
		else
			expr();
		if (prevlits.size() > 0)
			{
			PrevLit prev = poplits();
			if (! rec)
				rec = new SuRecord;
			if (! key)
				rec->put(argi, prev.lit);
			else
				{
				rec->put(symbol(argnames.back()), prev.lit);
				argnames.pop_back();
				}
			--nargs;
			}
		if (token == ',')
			match();
		}
	match(']');
	if (rec)
		{
		emit(I_PUSH, LITERAL, literal(rec));
		static int litrec = symnum("litrec");
		if (argnames.size() > 0)
			argnames.push_back(litrec);
		static int gMkRec = globals("mkrec");
		emit(I_CALL, GLOBAL, gMkRec, nargs + 1, &argnames);
		}
	else
		{
		static int gRecord = globals("Record");
		emit(I_CALL, GLOBAL, gRecord, nargs, &argnames);
		}
	}

short FunctionCompiler::literal(Value x)
	{
	except_if(literals.size() >= UCHAR_MAX, "too many literals");
	literals.push_back(x);
	return literals.size() - 1;
	}

short FunctionCompiler::local()
	{
	ushort num = symnum(scanner.value);
	for (int i = locals.size() - 1; i >= 0; --i)
		if (locals[i] == num)
			return i;
	except_if(locals.size() >= UCHAR_MAX, "too many local variables");
	locals.push_back(num);
	return locals.size() - 1;
	}

PrevLit FunctionCompiler::poplits()
	{
	PrevLit prev = prevlits.back();
	prevlits.pop_back();
	if (prev.il == literals.size() - 1)
		literals.pop_back();
	code.resize(prev.adr);
	return prev;
	}

short FunctionCompiler::emit(short op, short option, short target,
	short nargs, vector<ushort>* argnames)
	{
	short adr;

	// merge pop with previous instruction where possible
	if (last_adr >= 0 && op == I_POP)
		{
		adr = last_adr;
		short last_op = code[last_adr] & 0xf0;
		if (I_EQ_AUTO <= last_op && last_op < I_PUSH && ! (last_op & 8))
			{
			code[last_adr] += 8;
			return adr;
			}
		}

	// constant folding e.g. replace 2 * 4 with 8
	if (prevlits.size() >= 1 && (op == I_UMINUS || op == I_BITNOT))
		{
		PrevLit prev = poplits();
		Value result;
		if (op == I_UMINUS)
			result = - prev.lit;
		else if (op == I_BITNOT)
			result = ~ prev.lit.integer();
		op = I_PUSH;
		option = LITERAL;
		target = literal(result);
		}
	else if (prevlits.size() >= 2 && I_ADD <= op && op <= I_BITXOR)
		{
		PrevLit prev2 = poplits();
		PrevLit prev1 = poplits();
		Value result;
		switch (op)
			{
		case I_ADD : result = prev1.lit + prev2.lit; break ;
		case I_SUB : result = prev1.lit - prev2.lit; break ;
		case I_MUL : result = prev1.lit * prev2.lit; break ;
		case I_DIV : result = prev1.lit / prev2.lit; break ;
		case I_CAT : result = new SuString(prev1.lit.gcstr() + prev2.lit.gcstr()); break ;
		case I_MOD : result = prev1.lit.integer() % prev2.lit.integer(); break ;
		case I_LSHIFT : result = (ulong) prev1.lit.integer() << prev2.lit.integer(); break ;
		case I_RSHIFT : result = (ulong) prev1.lit.integer() >> prev2.lit.integer(); break ;
		case I_BITAND : result = (ulong) prev1.lit.integer() & (ulong) prev2.lit.integer(); break ;
		case I_BITOR : result = (ulong) prev1.lit.integer() | (ulong) prev2.lit.integer(); break ;
		case I_BITXOR : result = (ulong) prev1.lit.integer() ^ (ulong) prev2.lit.integer(); break ;
		default : unreachable();
			}
		op = I_PUSH;
		option = LITERAL;
		target = literal(result);
		}
	if (op == I_PUSH && option == LITERAL)
		{
		Value x = literals[target];
		prevlits.push_back(PrevLit(code.size(), x, target));
		int lit = target;
		if (x == SuOne)
			{ op = I_PUSH_VALUE; option = ONE; }
		else if (x == SuZero)
			{ op = I_PUSH_VALUE; option = ZERO; }
		else if (x == SuMinusOne)
			{ op = I_PUSH_VALUE; option = MINUS_ONE; }
		else if (x == SuEmptyString)
			{ op = I_PUSH_VALUE; option = EMPTY_STRING; }
		else if (x == SuTrue)
			{ op = I_PUSH_VALUE; option = TRUE; }
		else if (x == SuFalse)
			{ op = I_PUSH_VALUE; option = FALSE; }
		else if (x.is_int())
			{ op = I_PUSH_INT; target = x.integer(); }
		if (op != I_PUSH && lit == literals.size() - 1)
			{ 
			literals.pop_back(); 
			prevlits.back().il = -99; 
			}
		}
	else
		prevlits.clear();

	last_adr = adr = code.size();
	code.push_back(op | option);

	if (op == I_PUSH)
		{
		// OPTIMIZE: pushes
		if (option == LITERAL && target < 16)
			code[adr] = I_PUSH_LITERAL | target;
		else if (option == AUTO && target < 16)
			code[adr] = I_PUSH_AUTO | target;
		else if (option >= LITERAL)
			{
			code.push_back(target & 255);
			if (option >= MEM)
				{
				code.push_back(target >> 8);
				}
			}
		}

	else if (op == I_CALL)
		{
		if (option > LITERAL)	// literal means stack
			{
			code.push_back(target & 255);
			if (option >= MEM)
				code.push_back(target >> 8);
			}

		// OPTIMIZE: calls
		int nargnames = nargs && argnames ? argnames->size() : 0;
		if (option == GLOBAL && nargs < 8 && nargnames == 0)
			code[adr] = I_CALL_GLOBAL | nargs;
		else if (option == MEM && nargs < 8 && nargnames == 0)
			code[adr] = I_CALL_MEM | nargs;
		else if (option == MEM_SELF && nargs < 8 && nargnames == 0)
			code[adr] = I_CALL_MEM_SELF | nargs;
		else
			{
			code.push_back(nargs);
			code.push_back(nargnames);
			for (short i = 0; i < nargnames; ++i)
				{
				code.push_back((*argnames)[i]);
				code.push_back((*argnames)[i] >> 8);
				}
			}
		}

	else if (op == I_JUMP || op == I_TRY || op == I_CATCH ||
		op == I_SUPER || op == I_BLOCK || op == I_PUSH_INT)
		{
		code.push_back(target & 255);
		code.push_back(target >> 8);
		}

	else if (I_ADDEQ <= op && op <= I_POSTDEC)
		{
		option = ((option & 0x70) >> 4);
		if (op == I_EQ && option == AUTO && target < 8)
			code[adr] = I_EQ_AUTO | target;
		else if (option >= LITERAL)
			{
			code.push_back(target & 255);
			if (option >= MEM)
				code.push_back(target >> 8);
			}
		}
	
	else if (op == I_EACH)
		code.push_back(target);

	return adr;
	}

void FunctionCompiler::mark()
	{
	if (db.size() > 0 && db[db.size() - 1].ci + 1 == code.size())
		return ;
	db.push_back(Debug(scanner.prev, code.size()));
	emit(I_NOP);
	}

#define TARGET(i)	(short) ((code[i+1] + (code[i+2] << 8)))

void FunctionCompiler::patch(short i)
	{
	short adr, next;
	for (; i >= 0; i = next)
		{
		verify(i < code.size());
		next = TARGET(i);
		adr = code.size() - (i + 3);
		code[i+1] = adr & 255;
		code[i+2] = adr >> 8;
		last_adr = -1;	// prevent optimizations
		prevlits.clear();
		}
	}

// make lower case member names private by prefixing with class name
ushort Compiler::memname(char* gname, char* s)
	{
	if (gname && islower(s[0]))
		{
		if (has_prefix(s, "get_"))
			s = CATSTR3("Get_", gname, s + 3); // get_name => Get_Class_name
		else
			s = CATSTR3(gname, "_", s);
		}
	return symnum(s);
	}

#include "testing.h"
#include "ostreamstr.h"

const int BUFLEN = 10000;
char *buf = new char[BUFLEN];

struct Cmpltest
	{
	char* query;
	char* result;
	};

static Cmpltest cmpltests[] = 
	{
	{ "123;", "123; }\n\
					  0  nop \n\
					  1  push int 123\n\
					  4  return \n\
					  5\n" },

	{ "0377;", "0377; }\n\
					  0  nop \n\
					  1  push int 255\n\
					  4  return \n\
					  5\n" },

	{ "0x100;", "0x100; }\n\
					  0  nop \n\
					  1  push int 256\n\
					  4  return \n\
					  5\n" },

	{ "0x7fff;", "0x7fff; }\n\
					  0  nop \n\
					  1  push int 32767\n\
					  4  return \n\
					  5\n" },

	{ "0x10000;", "0x10000; }\n\
					  0  nop \n\
					  1  push literal 65536\n\
					  2  return \n\
					  3\n" },

	{ "0.001;", "0.001; }\n\
					  0  nop \n\
					  1  push literal .001\n\
					  2  return \n\
					  3\n" },

	{ "a;", "a; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  return \n\
					  3\n" },

	{ "a = 123;", "a = 123; }\n\
					  0  nop \n\
					  1  push int 123\n\
					  4  = auto a\n\
					  5  return \n\
					  6\n" },

	{ "a = b = c;","a = b = c; }\n\
					  0  nop \n\
					  1  push auto c\n\
					  2  = auto b\n\
					  3  = auto a\n\
					  4  return \n\
					  5\n" },

	{ "F();", "F(); }\n\
					  0  nop \n\
					  1  call global F 0\n\
					  4  return \n\
					  5\n" },

	{ "a.b();",	"a.b(); }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  call mem b 0\n\
					  5  return \n\
					  6\n" },

	{ ".b();", ".b(); }\n\
					  0  nop \n\
					  1  call mem this b 0\n\
					  4  return \n\
					  5\n" },

	{ "a[b];", "a[b]; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  push sub \n\
					  4  return \n\
					  5\n" },

/*	{ "[b];", "[b]; }\n\
					  0  nop \n\
					  1  push auto b\n\
					  2  push sub this \n\
					  3  return \n\
					  4\n" },*/

	{ "a.b;", "a.b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push mem b\n\
					  5  return \n\
					  6\n" },
					  
	{ ".b;", ".b; }\n\
					  0  nop \n\
					  1  push mem this b\n\
					  4  return \n\
					  5\n" },
					  
	{ "X;", "X; }\n\
					  0  nop \n\
					  1  push global X\n\
					  4  return \n\
					  5\n" }, 

	{ "True;", "True; }\n\
					  0  nop \n\
					  1  push value true \n\
					  2  return \n\
					  3\n" },

	{ "False;", "False; }\n\
					  0  nop \n\
					  1  push value false \n\
					  2  return \n\
					  3\n" },

	{ "\"\";", "\"\"; }\n\
					  0  nop \n\
					  1  push value \"\" \n\
					  2  return \n\
					  3\n" },

	{ "this;", "this; }\n\
					  0  nop \n\
					  1  push value this \n\
					  2  return \n\
					  3\n" },

	{ "-1;", "-1; }\n\
					  0  nop \n\
					  1  push value -1 \n\
					  2  return \n\
					  3\n" },

	{ "0;", "0; }\n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  return \n\
					  3\n" },

	{ "1;", "1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  return \n\
					  3\n" },

	{ "x = 1;", "x = 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  = auto x\n\
					  3  return \n\
					  4\n" },

	{ "x += 1;", "x += 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  += auto x\n\
					  4  return \n\
					  5\n" },

	{ "x -= 1;", "x -= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  -= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x $= \"\";", "x $= \"\"; }\n\
					  0  nop \n\
					  1  push value \"\" \n\
					  2  $= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x *= 1;", "x *= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  *= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x /= 1;", "x /= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  /= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x %= 1;", "x %= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  %= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x &= 1;", "x &= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  &= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x |= 1;", "x |= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  |= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x ^= 1;", "x ^= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  ^= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x <<= 1;", "x <<= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  <<= auto x\n\
					  4  return \n\
					  5\n" },

	{ "x >>= 1;", "x >>= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  >>= auto x\n\
					  4  return \n\
					  5\n" },

	{ "++x;", "++x; }\n\
					  0  nop \n\
					  1  ++? auto x\n\
					  3  return \n\
					  4\n" },

	{ "--x;", "--x; }\n\
					  0  nop \n\
					  1  --? auto x\n\
					  3  return \n\
					  4\n" },

	{ "x++;", "x++; }\n\
					  0  nop \n\
					  1  ?++ auto x\n\
					  3  return \n\
					  4\n" },

	{ "x--;", "x--; }\n\
					  0  nop \n\
					  1  ?-- auto x\n\
					  3  return \n\
					  4\n" },

	{ ".x = 1;", ".x = 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  = mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x += 1;", ".x += 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  += mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x -= 1;", ".x -= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  -= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x $= \"\";", ".x $= \"\"; }\n\
					  0  nop \n\
					  1  push value \"\" \n\
					  2  $= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x *= 1;", ".x *= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  *= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x /= 1;", ".x /= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  /= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x %= 1;", ".x %= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  %= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x &= 1;", ".x &= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  &= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x |= 1;", ".x |= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  |= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x ^= 1;", ".x ^= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  ^= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x <<= 1;", ".x <<= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  <<= mem this x\n\
					  5  return \n\
					  6\n" },

	{ ".x >>= 1;", ".x >>= 1; }\n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  >>= mem this x\n\
					  5  return \n\
					  6\n" },

	{ "++.x;", "++.x; }\n\
					  0  nop \n\
					  1  ++? mem this x\n\
					  4  return \n\
					  5\n" },

	{ "--.x;", "--.x; }\n\
					  0  nop \n\
					  1  --? mem this x\n\
					  4  return \n\
					  5\n" },

	{ ".x++;", ".x++; }\n\
					  0  nop \n\
					  1  ?++ mem this x\n\
					  4  return \n\
					  5\n" },

	{ ".x--;", ".x--; }\n\
					  0  nop \n\
					  1  ?-- mem this x\n\
					  4  return \n\
					  5\n" },

	{ "a + b;", "a + b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  + \n\
					  4  return \n\
					  5\n" },

	{ "a - b;", "a - b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  - \n\
					  4  return \n\
					  5\n" },

	{ "a $ b;", "a $ b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  $ \n\
					  4  return \n\
					  5\n" },

	{ "a * b;", "a * b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  * \n\
					  4  return \n\
					  5\n" },

	{ "a / b;", "a / b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  / \n\
					  4  return \n\
					  5\n" },

	{ "a % b;", "a % b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  % \n\
					  4  return \n\
					  5\n" },

	{ "a & b;", "a & b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  & \n\
					  4  return \n\
					  5\n" },

	{ "a | b;", "a | b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  | \n\
					  4  return \n\
					  5\n" },

	{ "a ^ b;", "a ^ b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  ^ \n\
					  4  return \n\
					  5\n" },

	{ "a == b;", "a == b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  == \n\
					  4  return \n\
					  5\n" },

	{ "a != b;", "a != b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  != \n\
					  4  return \n\
					  5\n" },

	{ "a =~ b;", "a =~ b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  =~ \n\
					  4  return \n\
					  5\n" },

	{ "a !~ b;", "a !~ b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  !~ \n\
					  4  return \n\
					  5\n" },

	{ "a < b;", "a < b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  < \n\
					  4  return \n\
					  5\n" },

	{ "a <= b;", "a <= b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  <= \n\
					  4  return \n\
					  5\n" },

	{ "a > b;", "a > b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  > \n\
					  4  return \n\
					  5\n" },

	{ "a >= b;", "a >= b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  >= \n\
					  4  return \n\
					  5\n" },

	{ "! a;", "! a; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  ! \n\
					  3  return \n\
					  4\n" },

	{ "- a;", "- a; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  -? \n\
					  3  return \n\
					  4\n" },

	{ "~ a;", "~ a; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  ~ \n\
					  3  return \n\
					  4\n" },

	{ "a ? b : c;", "a ? b : c; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 9\n\
					  5  push auto b\n\
					  6  jump 10\n\
					  9  push auto c\n\
					 10  return \n\
					 11\n" },

	{ "a ? b ? c : d : e ? f : g;", "a ? b ? c : d : e ? f : g; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 17\n\
					  5  push auto b\n\
					  6  jump pop no 13\n\
					  9  push auto c\n\
					 10  jump 14\n\
					 13  push auto d\n\
					 14  jump 26\n\
					 17  push auto e\n\
					 18  jump pop no 25\n\
					 21  push auto f\n\
					 22  jump 26\n\
					 25  push auto g\n\
					 26  return \n\
					 27\n" },

	{ "a || b || c;", "a || b || c; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump else pop yes 10\n\
					  5  push auto b\n\
					  6  jump else pop yes 10\n\
					  9  push auto c\n\
					 10  return \n\
					 11\n" },

	{ "a && b && c;", "a && b && c; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump else pop no 10\n\
					  5  push auto b\n\
					  6  jump else pop no 10\n\
					  9  push auto c\n\
					 10  return \n\
					 11\n" },

	{ "a < b + c * -d;", "a < b + c * -d; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  push auto c\n\
					  4  push auto d\n\
					  5  -? \n\
					  6  * \n\
					  7  + \n\
					  8  < \n\
					  9  return \n\
					 10\n" },

	{ ".a;", ".a; }\n\
					  0  nop \n\
					  1  push mem this a\n\
					  4  return \n\
					  5\n" },

/*	{ "[a];", "[a]; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push sub this \n\
					  3  return \n\
					  4\n" },*/

	{ "(a);", "(a); }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  return \n\
					  3\n" },

	{ "a.b;", "a.b; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push mem b\n\
					  5  return \n\
					  6\n" },

	{ "a[b];", "a[b]; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  push sub \n\
					  4  return \n\
					  5\n" },

	{ "a();", "a(); }\n\
					  0  nop \n\
					  1  call auto a 0 0\n\
					  5  return \n\
					  6\n" },

	{ "a.b.c.d;", "a.b.c.d; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push mem b\n\
					  5  push mem c\n\
					  8  push mem d\n\
					 11  return \n\
					 12\n" },

	{ "a[b][c][d];", "a[b][c][d]; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  push auto b\n\
					  3  push sub \n\
					  4  push auto c\n\
					  5  push sub \n\
					  6  push auto d\n\
					  7  push sub \n\
					  8  return \n\
					  9\n" },

	{ "++a; --a; a++; a--;", "++a; \n\
					  0  nop \n\
					  1  ++? auto a\n\
					  3  pop \n\
--a; \n\
					  4  nop \n\
					  5  --? auto a\n\
					  7  pop \n\
a++; \n\
					  8  nop \n\
					  9  ?++ auto a\n\
					 11  pop \n\
a--; }\n\
					 12  nop \n\
					 13  ?-- auto a\n\
					 15  return \n\
					 16\n" },

	{ "a().b;", "a().b; }\n\
					  0  nop \n\
					  1  call auto a 0 0\n\
					  5  push mem b\n\
					  8  return \n\
					  9\n" },

	{ "a()()();", "a()()(); }\n\
					  0  nop \n\
					  1  call auto a 0 0\n\
					  5  call stack  0 0\n\
					  8  call stack  0 0\n\
					 11  return \n\
					 12\n" },

	{ "a.b();", "a.b(); }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  call mem b 0\n\
					  5  return \n\
					  6\n" },

	{ "a = b;  a.b = c.d; a().b = c;", \
	  "a = b;  \n\
					  0  nop \n\
					  1  push auto b\n\
					  2  = auto pop a\n\
a.b = c.d; \n\
					  3  nop \n\
					  4  push auto a\n\
					  5  push auto c\n\
					  6  push mem d\n\
					  9  = mem b\n\
					 12  pop \n\
a().b = c; }\n\
					 13  nop \n\
					 14  call auto a 0 0\n\
					 18  push auto c\n\
					 19  = mem b\n\
					 22  return \n\
					 23\n" },

	
	{ "a(); a(b); a(b()); a(b, c, d); a(b(), c());", \
	  "a(); \n\
					  0  nop \n\
					  1  call auto a 0 0\n\
					  5  pop \n\
a(b); \n\
					  6  nop \n\
					  7  push auto b\n\
					  8  call auto a 1 0\n\
					 12  pop \n\
a(b()); \n\
					 13  nop \n\
					 14  call auto b 0 0\n\
					 18  call auto a 1 0\n\
					 22  pop \n\
a(b, c, d); \n\
					 23  nop \n\
					 24  push auto b\n\
					 25  push auto c\n\
					 26  push auto d\n\
					 27  call auto a 3 0\n\
					 31  pop \n\
a(b(), c()); }\n\
					 32  nop \n\
					 33  call auto b 0 0\n\
					 37  call auto c 0 0\n\
					 41  call auto a 2 0\n\
					 45  return \n\
					 46\n" },


	{ "a(b: 1); a(b: 0, c: 1); a(b, c, d: 0, e: 1);",
"a(b: 1); \n\
					  0  nop \n\
					  1  push value 1 \n\
					  2  call auto a 1 1 b\n\
					  8  pop \n\
a(b: 0, c: 1); \n\
					  9  nop \n\
					 10  push value 0 \n\
					 11  push value 1 \n\
					 12  call auto a 2 2 b c\n\
					 20  pop \n\
a(b, c, d: 0, e: 1); }\n\
					 21  nop \n\
					 22  push auto b\n\
					 23  push auto c\n\
					 24  push value 0 \n\
					 25  push value 1 \n\
					 26  call auto a 4 2 d e\n\
					 34  return \n\
					 35\n" },

	{ "a(@args);", "a(@args); }\n\
					  0  nop \n\
					  1  push auto args\n\
					  2  each 0\n\
					  4  call auto a 1 0\n\
					  8  return \n\
					  9\n" },

	{ "if (a) b;", "if (a) \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 8\n\
b; }\n\
					  5  nop \n\
					  6  push auto b\n\
					  7  pop \n\
\n\
					  8  nop \n\
					  9  return nil \n\
					 10\n" },

	{ "if (a) b; else c;", "if (a) \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 11\n\
b; \n\
					  5  nop \n\
					  6  push auto b\n\
					  7  pop \n\
					  8  jump 14\n\
else c; }\n\
					 11  nop \n\
					 12  push auto c\n\
					 13  pop \n\
\n\
					 14  nop \n\
					 15  return nil \n\
					 16\n" },

	{ "if (a) b; else if (c) d; else e;", "if (a) \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 11\n\
b; \n\
					  5  nop \n\
					  6  push auto b\n\
					  7  pop \n\
					  8  jump 25\n\
else if (c) \n\
					 11  nop \n\
					 12  push auto c\n\
					 13  jump pop no 22\n\
d; \n\
					 16  nop \n\
					 17  push auto d\n\
					 18  pop \n\
					 19  jump 25\n\
else e; }\n\
					 22  nop \n\
					 23  push auto e\n\
					 24  pop \n\
\n\
					 25  nop \n\
					 26  return nil \n\
					 27\n" },

	{ "if (a) b; else if (c) d; else if (e) f;", "if (a) \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 11\n\
b; \n\
					  5  nop \n\
					  6  push auto b\n\
					  7  pop \n\
					  8  jump 30\n\
else if (c) \n\
					 11  nop \n\
					 12  push auto c\n\
					 13  jump pop no 22\n\
d; \n\
					 16  nop \n\
					 17  push auto d\n\
					 18  pop \n\
					 19  jump 30\n\
else if (e) \n\
					 22  nop \n\
					 23  push auto e\n\
					 24  jump pop no 30\n\
f; }\n\
					 27  nop \n\
					 28  push auto f\n\
					 29  pop \n\
\n\
					 30  nop \n\
					 31  return nil \n\
					 32\n" },

	{ "forever\n F();", "forever  F(); }\n\
					  0  nop \n\
					  1  call global pop F 0\n\
					  4  jump 0\n\
\n\
					  7  nop \n\
					  8  return nil \n\
					  9\n" },

	{ "while (a)\n F();", "while (a)  \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 12\n\
F(); }\n\
					  5  nop \n\
					  6  call global pop F 0\n\
					  9  jump 0\n\
\n\
					 12  nop \n\
					 13  return nil \n\
					 14\n" },

	{ "do\n F();\n while (b);", "do  \n\
					  0  nop \n\
					  1  jump 7\n\
					  4  jump 11\n\
F();  \n\
					  7  nop \n\
					  8  call global pop F 0\n\
while (b); }\n\
					 11  nop \n\
					 12  push auto b\n\
					 13  jump pop yes 7\n\
\n\
					 16  nop \n\
					 17  return nil \n\
					 18\n" },

	{ "for (i = 0; i < 8; ++i)\n F();", "for (i = 0; i < 8; ++i)  \n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  = auto pop i\n\
					  3  jump 9\n\
					  6  ++? auto i\n\
					  8  pop \n\
					  9  push auto i\n\
					 10  push int 8\n\
					 13  < \n\
					 14  jump pop no 24\n\
F(); }\n\
					 17  nop \n\
					 18  call global pop F 0\n\
					 21  jump 6\n\
\n\
					 24  nop \n\
					 25  return nil \n\
					 26\n" },

	{ "for (i = 0; i < 8; ++i)\n ;", "for (i = 0; i < 8; ++i)  \n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  = auto pop i\n\
					  3  jump 9\n\
					  6  ++? auto i\n\
					  8  pop \n\
					  9  push auto i\n\
					 10  push int 8\n\
					 13  < \n\
					 14  jump pop no 21\n\
; }\n\
					 17  nop \n\
					 18  jump 6\n\
\n\
					 21  nop \n\
					 22  return nil \n\
					 23\n" },

	{ "for (i = 0; ; ++i)\n F();", "for (i = 0; ; ++i)  \n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  = auto pop i\n\
					  3  jump 9\n\
					  6  ++? auto i\n\
					  8  pop \n\
F(); }\n\
					  9  nop \n\
					 10  call global pop F 0\n\
					 13  jump 6\n\
\n\
					 16  nop \n\
					 17  return nil \n\
					 18\n" },

	{ "for (i = 0; i < 8; )\n F();", "for (i = 0; i < 8; )  \n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  = auto pop i\n\
					  3  push auto i\n\
					  4  push int 8\n\
					  7  < \n\
					  8  jump pop no 18\n\
F(); }\n\
					 11  nop \n\
					 12  call global pop F 0\n\
					 15  jump 3\n\
\n\
					 18  nop \n\
					 19  return nil \n\
					 20\n" },

	{ "for (i = 0; ; )\n F();", "for (i = 0; ; )  \n\
					  0  nop \n\
					  1  push value 0 \n\
					  2  = auto pop i\n\
F(); }\n\
					  3  nop \n\
					  4  call global pop F 0\n\
					  7  jump 3\n\
\n\
					 10  nop \n\
					 11  return nil \n\
					 12\n" },

	{ "for (; i < 8; )\n F();", "for (; i < 8; )  \n\
					  0  nop \n\
					  1  push auto i\n\
					  2  push int 8\n\
					  5  < \n\
					  6  jump pop no 16\n\
F(); }\n\
					  9  nop \n\
					 10  call global pop F 0\n\
					 13  jump 1\n\
\n\
					 16  nop \n\
					 17  return nil \n\
					 18\n" },

	{ "for (; ; ++i)\n F();", "for (; ; ++i)  \n\
					  0  nop \n\
					  1  jump 7\n\
					  4  ++? auto i\n\
					  6  pop \n\
F(); }\n\
					  7  nop \n\
					  8  call global pop F 0\n\
					 11  jump 4\n\
\n\
					 14  nop \n\
					 15  return nil \n\
					 16\n" },

	{ "for (; ; )\n F();", "for (; ; )  F(); }\n\
					  0  nop \n\
					  1  call global pop F 0\n\
					  4  jump 0\n\
\n\
					  7  nop \n\
					  8  return nil \n\
					  9\n" },

	{ "forever\n {\n if (a)\n continue;\n if (b)\n continue ;\n if (c)\n break ;\n if (d)\n break ;\n}" , \
	  "forever  {  if (a)  \n\
					  0  nop \n\
					  1  push auto a\n\
					  2  jump pop no 9\n\
continue;  \n\
					  5  nop \n\
					  6  jump 0\n\
if (b)  \n\
					  9  nop \n\
					 10  push auto b\n\
					 11  jump pop no 18\n\
continue ;  \n\
					 14  nop \n\
					 15  jump 0\n\
if (c)  \n\
					 18  nop \n\
					 19  push auto c\n\
					 20  jump pop no 27\n\
break ;  \n\
					 23  nop \n\
					 24  jump 39\n\
if (d)  \n\
					 27  nop \n\
					 28  push auto d\n\
					 29  jump pop no 36\n\
break ; } }\n\
					 32  nop \n\
					 33  jump 39\n\
					 36  jump 0\n\
\n\
					 39  nop \n\
					 40  return nil \n\
					 41\n" },

	{ "return ;", "return ; }\n\
					  0  nop \n\
					  1  return nil \n\
					  2\n" },

	{ "return a;", "return a; }\n\
					  0  nop \n\
					  1  push auto a\n\
					  2  return \n\
					  3\n" },

	{ "switch (a)\n {\n case 1 :\n b();\n case 2, 3, 4 :\n c();\n }", \
	  "switch (a)  {  \n\
					  0  nop \n\
					  1  push auto a\n\
case 1 :  \n\
					  2  nop \n\
					  3  push value 1 \n\
					  4  jump case no 16\n\
b();  \n\
					  7  nop \n\
					  8  call auto b 0 0\n\
					 12  pop \n\
					 13  jump 45\n\
case 2, 3, 4 :  \n\
					 16  nop \n\
					 17  push int 2\n\
					 20  jump case yes 35\n\
					 23  push int 3\n\
					 26  jump case yes 35\n\
					 29  push int 4\n\
					 32  jump case no 44\n\
c();  } }\n\
					 35  nop \n\
					 36  call auto c 0 0\n\
					 40  pop \n\
					 41  jump 45\n\
					 44  pop \n\
\n\
					 45  nop \n\
					 46  return nil \n\
					 47\n" },

	{ "switch (a)\n { case 1 :\n b();\n case 2, 3, 4 :\n c();\n default :\n d();\n }", \
	  "switch (a)  { \n\
					  0  nop \n\
					  1  push auto a\n\
case 1 :  \n\
					  2  nop \n\
					  3  push value 1 \n\
					  4  jump case no 16\n\
b();  \n\
					  7  nop \n\
					  8  call auto b 0 0\n\
					 12  pop \n\
					 13  jump 56\n\
case 2, 3, 4 :  \n\
					 16  nop \n\
					 17  push int 2\n\
					 20  jump case yes 35\n\
					 23  push int 3\n\
					 26  jump case yes 35\n\
					 29  push int 4\n\
					 32  jump case no 44\n\
c();  \n\
					 35  nop \n\
					 36  call auto c 0 0\n\
					 40  pop \n\
					 41  jump 56\n\
default :  \n\
					 44  nop \n\
					 45  pop \n\
d();  } }\n\
					 46  nop \n\
					 47  call auto d 0 0\n\
					 51  pop \n\
					 52  jump 56\n\
					 55  pop \n\
\n\
					 56  nop \n\
					 57  return nil \n\
					 58\n" },

	{ "#();", "#(); }\n\
					  0  nop \n\
					  1  push literal #()\n\
					  2  return \n\
					  3\n" },

	{ "#(1);", "#(1); }\n\
					  0  nop \n\
					  1  push literal #(1)\n\
					  2  return \n\
					  3\n" },

	{ "#(1, 2, 3);", "#(1, 2, 3); }\n\
					  0  nop \n\
					  1  push literal #(1, 2, 3)\n\
					  2  return \n\
					  3\n" },

	{ "#(a: 1);", "#(a: 1); }\n\
					  0  nop \n\
					  1  push literal #(a: 1)\n\
					  2  return \n\
					  3\n" },

	{ "#(0, b: 1);", "#(0, b: 1); }\n\
					  0  nop \n\
					  1  push literal #(0, b: 1)\n\
					  2  return \n\
					  3\n" },
/*
	{ "#(a: 0, b: 1, c: 2);", "#(a: 0, b: 1, c: 2); }\n\
					  0  nop \n\
					  1  push literal #(b: 1, a: 0, c: 2)\n\
					  2  return \n\
					  3\n" },
*/
	{ "function () { };", "function () { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (a) { };", "function (a) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (a, b, c) { };", "function (a, b, c) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (a = 1) { };", "function (a = 1) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (a = 1, b = 2, c = 3) { };", "function (a = 1, b = 2, c = 3) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (a, b, c = 0, d = 1) { };", "function (a, b, c = 0, d = 1) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "function (@args) { };", "function (@args) { }; }\n\
					  0  nop \n\
					  1  push literal  /*  function */\n\
					  2  return \n\
					  3\n" },

	{ "class { };", "class { }; }\n\
					  0  nop \n\
					  1  push literal  /* class */\n\
					  2  return \n\
					  3\n" },

	{ "class : X { };", "class : X { }; }\n\
					  0  nop \n\
					  1  push literal  /* class : X */\n\
					  2  return \n\
					  3\n" },

	{ "class { a: 1 };", "class { a: 1 }; }\n\
					  0  nop \n\
					  1  push literal  /* class */\n\
					  2  return \n\
					  3\n" },
	{ "class { a: 1, b: 2 };", "class { a: 1, b: 2 }; }\n\
					  0  nop \n\
					  1  push literal  /* class */\n\
					  2  return \n\
					  3\n" },

	{ "class { a: 1; b: 2; };", "class { a: 1; b: 2; }; }\n\
					  0  nop \n\
					  1  push literal  /* class */\n\
					  2  return \n\
					  3\n" },

	{ "class { a: 1, b: 2, c: 3 };", "class { a: 1, b: 2, c: 3 }; }\n\
					  0  nop \n\
					  1  push literal  /* class */\n\
					  2  return \n\
					  3\n" },

	{ "-7;", "-7; }\n\
					  0  nop \n\
					  1  push int -7\n\
					  4  return \n\
					  5\n" },

	{ "-7.0;", "-7.0; }\n\
					  0  nop \n\
					  1  push literal -7\n\
					  2  return \n\
					  3\n" },

	{ "~5;", "~5; }\n\
					  0  nop \n\
					  1  push int -6\n\
					  4  return \n\
					  5\n" },

	{ "6 + 0;", "6 + 0; }\n\
					  0  nop \n\
					  1  push int 6\n\
					  4  return \n\
					  5\n" },

	{ "6.0 + 3.0;", "6.0 + 3.0; }\n\
					  0  nop \n\
					  1  push literal 9\n\
					  2  return \n\
					  3\n" },

	{ "6 - 3;", "6 - 3; }\n\
					  0  nop \n\
					  1  push int 3\n\
					  4  return \n\
					  5\n" },

	{ "6.0 - 3.0;", "6.0 - 3.0; }\n\
					  0  nop \n\
					  1  push literal 3\n\
					  2  return \n\
					  3\n" },

	{ "6 * 3;", "6 * 3; }\n\
					  0  nop \n\
					  1  push int 18\n\
					  4  return \n\
					  5\n" },

	{ "6.0 * 3.0;", "6.0 * 3.0; }\n\
					  0  nop \n\
					  1  push literal 18\n\
					  2  return \n\
					  3\n" },

	{ "6 / 3;", "6 / 3; }\n\
					  0  nop \n\
					  1  push int 2\n\
					  4  return \n\
					  5\n" },

	{ "6.0 / 3.0;", "6.0 / 3.0; }\n\
					  0  nop \n\
					  1  push literal 2\n\
					  2  return \n\
					  3\n" },

	{ "6 % 4;", "6 % 4; }\n\
					  0  nop \n\
					  1  push int 2\n\
					  4  return \n\
					  5\n" },

	{ "5 & 6;", "5 & 6; }\n\
					  0  nop \n\
					  1  push int 4\n\
					  4  return \n\
					  5\n" },

	{ "5 | 6;", "5 | 6; }\n\
					  0  nop \n\
					  1  push int 7\n\
					  4  return \n\
					  5\n" },

	{ "5 ^ 6;", "5 ^ 6; }\n\
					  0  nop \n\
					  1  push int 3\n\
					  4  return \n\
					  5\n" },

	{ "1 << 4;", "1 << 4; }\n\
					  0  nop \n\
					  1  push int 16\n\
					  4  return \n\
					  5\n" },

	{ "32 >> 2;", "32 >> 2; }\n\
					  0  nop \n\
					  1  push int 8\n\
					  4  return \n\
					  5\n" },

	{ "'hello' $ '';", "'hello' $ ''; }\n\
					  0  nop \n\
					  1  push literal \"hello\"\n\
					  2  return \n\
					  3\n" },

	{ "(true ? 'xx' : 'yy') $ 'z'", "(true ? 'xx' : 'yy') $ 'z' }\n\
					  0  nop \n\
					  1  push value true \n\
					  2  jump pop no 9\n\
					  5  push literal \"xx\"\n\
					  6  jump 10\n\
					  9  push literal \"yy\"\n\
					 10  push literal \"z\"\n\
					 11  $ \n\
					 12  return \n\
					 13\n" },

	{ "-(true ? 123 : 456)", "-(true ? 123 : 456) }\n\
					  0  nop \n\
					  1  push value true \n\
					  2  jump pop no 11\n\
					  5  push int 123\n\
					  8  jump 14\n\
					 11  push int 456\n\
					 14  -? \n\
					 15  return \n\
					 16\n" }
	};

class test_compile : public Tests
	{
	TEST(0, compile)
		{
		int i;
		for (i = 0; i < sizeof cmpltests / sizeof (Cmpltest); ++i)
			process(i, cmpltests[i].query, cmpltests[i].result);
		}

	void process(int i, char* code, char* result);
	TEST(1, function)
		{
		char* s = "function (a, b = 0, c = False, d = 123, e = 'hello') { }";
		SuFunction* fn = force<SuFunction*>(compile(s));
		verify(fn->nparams == 5);
		verify(fn->ndefaults == 4);
		verify(fn->locals[0] == symnum("a"));
		verify(fn->locals[1] == symnum("b"));
		verify(fn->locals[2] == symnum("c"));
		verify(fn->locals[3] == symnum("d"));
		verify(fn->locals[4] == symnum("e"));
		verify(fn->literals[0] == SuZero);
		verify(fn->literals[1] == SuFalse);
		verify(fn->literals[2] == 123);
		verify(fn->literals[3] == Value("hello"));
		}
	};
REGISTER(test_compile);

void test_compile::process(int i, char* code, char* result)
	{
	char buf[4000];
	strcpy(buf, "function () {\n");
	strcat(buf, code);
	strcat(buf, "\n}");
	SuFunction* fn = force<SuFunction*>(compile(buf));
	OstreamStr out;
	fn->disasm(out);
	const char* output = out.str();
	if (0 != strcmp(result, output))
		except(i << ": " << code << "\n\t=> " << result << "\n\t!= '" << output << "'");
	}

class test_compile_params : public Tests
	{
	TEST(0, main)
		{
		Params* p;
		
		p = compile_params("");
		asserteq(p->nrequired, 0);
		asserteq(p->ndefaults, 0);
		asserteq(p->rest, 0);
		verify(p->names == 0);
		verify(p->defaults == 0);
		
		p = compile_params("a,b,c");
		asserteq(p->nrequired, 3);
		asserteq(p->ndefaults, 0);
		asserteq(p->rest, 0);
		asserteq(p->names[0], symnum("a"));
		asserteq(p->names[1], symnum("b"));
		asserteq(p->names[2], symnum("c"));
		verify(p->defaults == 0);
		
		p = compile_params("a,b,c=12,d='D',@e");
		asserteq(p->nrequired, 2);
		asserteq(p->ndefaults, 2);
		asserteq(p->rest, 1);
		asserteq(p->names[0], symnum("a"));
		asserteq(p->names[1], symnum("b"));
		asserteq(p->names[2], symnum("c"));
		asserteq(p->names[3], symnum("d"));
		asserteq(p->names[4], symnum("e"));
		asserteq(p->defaults[0], 12);
		asserteq(p->defaults[1].gcstr(), "D");
		}
	};
REGISTER(test_compile_params);

class test_compile2 : public Tests
	{
	TEST(0, main)
		{
		compile("function (x) { if x\n{ } }");
		compile("function () {\n for x in F()\n { } }");
		compile("function () { x \n * y }");
		compile("function () { x \n ? y : z }");
		compile("function () { [1, x, a: y, b: 4] }");
		}
	};
REGISTER(test_compile2);
