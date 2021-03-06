#pragma once
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *\
* This file is part of Suneido - The Integrated Application Platform
* see: http://www.suneido.com for more information.
*
* Copyright (c) 2017 Suneido Software Corp.
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
#include "gcstring.h"

class CharMatcher
	{
public:
	virtual ~CharMatcher() = default;
	virtual bool matches(char ch) const;
	CharMatcher* negate();
	CharMatcher* or_(CharMatcher* cm);

	static CharMatcher NONE;
	static CharMatcher* is(char c);
	static CharMatcher* anyOf(const gcstring& chars);
	static CharMatcher* noneOf(const gcstring& chars);
	static CharMatcher* inRange(unsigned from, unsigned to);
protected:
	int indexIn(const gcstring& s, int start) const;
	};
