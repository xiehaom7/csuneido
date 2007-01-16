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

#include "interp.h"
#include "symbols.h"
#include "suboolean.h"
#include "sunumber.h"
#include "sustring.h"
#include "ostream.h"
#include <objbase.h>

inline LPWSTR WINAPI AtlA2WHelper(LPWSTR lpw, LPCSTR lpa, int nChars)
	{
	lpw[0] = '\0';
	MultiByteToWideChar(CP_ACP, 0, lpa, -1, lpw, nChars);
	return lpw;
	}
#define USES_CONVERSION \
	int _convert; (void) _convert;\
	char* _lpa; (void) _lpa;
#define A2OLE(s) \
	(((_lpa = s) == NULL) ? NULL : ( _convert = (strlen(_lpa)+1), AtlA2WHelper((LPWSTR) _alloca(_convert*2), _lpa, _convert)))

// a wrapper for COM objects
// that allows access via IDispatch
class SuCOMobject : public  SuValue
	{
public:
	SuCOMobject(IDispatch* id, char* pi = "???") : idisp(id), progid(pi)
		{ }
	void out(Ostream& os);
	Value call(Value self, Value member, short nargs, short nargnames, ushort* argnames, int each);
	// properties
	Value getdata(Value);
	void putdata(Value, Value);
	IDispatch* idispatch()
		{ return idisp; }
private:
	IDispatch* idisp;
	char* progid;
	};

void SuCOMobject::out(Ostream& os)
	{
	os << "COMobject('" << progid << "')";
	}

static void check_result(HRESULT hr, char* progid, char* name, char* action)
	{
	if (hr == DISP_E_BADPARAMCOUNT)
		except("COM: " << progid << " " << action << " " << name << " bad param count");
	else if (hr == DISP_E_BADVARTYPE)
		except("COM: " << progid << " " << action << " " << name << " bad var type");
	else if (hr == DISP_E_EXCEPTION)
		except("COM: " << progid << " " << action << " " << name << " exception");
	else if (hr == DISP_E_MEMBERNOTFOUND)
		except("COM: " << progid << " " << action << " " << name << " member not found");
	else if (hr == DISP_E_NONAMEDARGS)
		except("COM: " << progid << " " << action << " " << name << " no named args");
	else if (hr == DISP_E_OVERFLOW)
		except("COM: " << progid << " " << action << " " << name << " overflow");
	else if (hr == DISP_E_PARAMNOTFOUND)
		except("COM: " << progid << " " << action << " " << name << " param not found");
	else if (hr == DISP_E_TYPEMISMATCH)
		except("COM: " << progid << " " << action << " " << name << " type mismatch");
	else if (hr == DISP_E_PARAMNOTFOUND)
		except("COM: " << progid << " " << action << " " << name << " param not found");
	else if (hr == DISP_E_PARAMNOTOPTIONAL)
		except("COM: " << progid << " " << action << " " << name << " param not optional");
	else if (FAILED(hr))
		except("COM: " << progid << " " << action << " " << name << " failed");
	}

static Value com2su(VARIANT* var)
	{
	HRESULT hr;
	Value result = 0;

	/* get a fully dereferenced copy of the variant */
	/* ### we may want to optimize this sometime... avoid copying values */
	VARIANT varValue;
	VariantInit(&varValue);
	VariantCopyInd(&varValue, var);

	USES_CONVERSION;
	switch (V_VT(&varValue))
		{
	case VT_BOOL:
		result = V_BOOL(&varValue) == 0 ? SuFalse : SuTrue;
		break ;
	case VT_I2:
		result = V_I2(&varValue);
		break;
	case VT_I4:
		result = V_I4(&varValue);
		break;
	case VT_R4:
		result = SuNumber::from_float(V_R4(&varValue));
		break ;
	case VT_R8:
		result = SuNumber::from_double(V_R8(&varValue));
		break ;
	case VT_DISPATCH:
		result = new SuCOMobject(V_DISPATCH(&varValue));
		break;
	case VT_UNKNOWN:
		{
		IUnknown* iunk = V_UNKNOWN(&varValue);
		IDispatch* idisp = 0;
		hr = iunk->QueryInterface(IID_IDispatch, (void**) &idisp);
		if (FAILED(hr) || ! idisp)
			except("COM: couldn't convert UNKNOWN");
		result = new SuCOMobject(V_DISPATCH(&varValue));
		break;
		}
	case VT_NULL:
	case VT_EMPTY:
		result = 0;
		break;
	case VT_BSTR:
		{
		int nw = SysStringLen(V_BSTR(&varValue));
		if (nw == 0)
			return SuString::empty_string;
		int n = WideCharToMultiByte(CP_ACP, 0, V_BSTR(&varValue), nw, 0, 0, NULL, NULL);
		char* s = (char*) alloca(n);
		n = WideCharToMultiByte(CP_ACP, 0, V_BSTR(&varValue), nw, s, n, NULL, NULL);
		if (n == 0)
			except("COM: string conversion error");
		result = new SuString(s, n);
		break;
		}
	default:
		except("COM: can't convert to Suneido value");
		}

	VariantClear(&varValue);
	return result;
	}

Value SuCOMobject::getdata(Value member)
	{
	if (! idisp)
		except("COM: " << progid << " already released");
	// get id from name
	char* name = member.str();
	USES_CONVERSION;
	OLECHAR* wname = A2OLE(name);
	DISPID dispid;
	HRESULT hr = idisp->GetIDsOfNames(IID_NULL, &wname, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
	if (FAILED(hr))
		except("COM: " << progid << " doesn't have " << name);
	// convert args
	DISPPARAMS args = { NULL, NULL, 0, 0 };
	// invoke
	VARIANT result;
	hr = idisp->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET,
		&args, &result, NULL, NULL);
	check_result(hr, progid, name, "get");
	// convert return value
	return com2su(&result);
	}

inline BSTR A2WBSTR(LPCSTR lp, int nLen = -1)
	{
	int n = MultiByteToWideChar(CP_ACP, 0, lp, nLen, NULL, 0) - 1;
	BSTR s = ::SysAllocStringLen(NULL, n);
	if (s != NULL)
		MultiByteToWideChar(CP_ACP, 0, lp, -1, s, n);
	return s;
	}

static void su2com(Value x, VARIANT* v)
	{
	int n;
	const char* s;
	if (x == SuTrue || x == SuFalse)
		{
		V_VT(v) = VT_BOOL;
		V_BOOL(v) = (x == SuTrue ? VARIANT_TRUE : VARIANT_FALSE);
		}
	else if (x.int_if_num(&n))
		{
		V_VT(v) = VT_I4;
		V_I4(v) = n;
		}
	else if (SuNumber* n = val_cast<SuNumber*>(x))
		{
		V_VT(v) = VT_R8;
		V_R8(v) = n->to_double();
		}
	else if ((s = x.str_if_str()))
		{
		V_VT(v) = VT_BSTR;
		V_BSTR(v) = A2WBSTR(s);
		// TODO: handle strings with embedded nuls
		// TODO: THIS SHOULD BE FREE'D WITH SysFreeString
		}
	else if (SuCOMobject* sco = val_cast<SuCOMobject*>(x))
		{
		V_VT(v) = VT_DISPATCH;
		V_DISPATCH(v) = sco->idispatch();
		}
	else
		except("COM: can't convert: " << x);
	}

void SuCOMobject::putdata(Value member, Value val)
	{
	if (! idisp)
		except("COM: " << progid << " already released");
	// get id from name
	char* name = member.str();
	USES_CONVERSION;
	OLECHAR* wname = A2OLE(name);
	DISPID dispid;
	HRESULT hr = idisp->GetIDsOfNames(IID_NULL, &wname, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
	if (FAILED(hr))
		except("COM: " << progid << " doesn't have " << name);
	// convert args
	VARIANTARG arg;
	su2com(val, &arg);
    // Property puts have named arg that represents the value being assigned to the property.
    DISPID put = DISPID_PROPERTYPUT;
	DISPPARAMS args = { &arg, &put, 1, 1 };
	// invoke
	hr = idisp->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYPUT,
		&args, NULL, NULL, NULL);
	check_result(hr, progid, name, "put");
	}

Value SuCOMobject::call(Value self, Value member, short nargs, short nargnames, ushort* argnames, int each)
	{
	if (! idisp)
		except("COM: " << progid << " already released");
	static Value RELEASE("Release");
	if (member == RELEASE)
		{
		if (nargs != 0)
			except("usage: comobject.Release()");
		idisp->Release();
		idisp = 0;
		return Value();
		}
	// else call

	// get id from name
	char* name = member.str();
	USES_CONVERSION;
	OLECHAR* wname = A2OLE(name);
	DISPID dispid;
	HRESULT hr = idisp->GetIDsOfNames(IID_NULL, &wname, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
	if (FAILED(hr))
		except("COM: " << progid << " doesn't have " << name);

	// convert args
	DISPPARAMS args = { NULL, NULL, 0, 0 };
	args.cArgs = nargs;
	VARIANT* vargs = (VARIANT*) alloca(nargs * sizeof (VARIANT));
	for (int i = 0; i < nargs; ++i)
		su2com(ARG(i), &vargs[nargs - i - 1]);
	args.rgvarg = vargs;

	VARIANT result;
	VariantInit(&result);
	hr = idisp->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
		&args, &result, NULL, NULL);
	check_result(hr, progid, name, "call");

	return com2su(&result);
	}

// su_COMobject() ===================================================

#include "prim.h"

Value su_COMobject()
	{
	const int nargs = 1;
	HRESULT hr;
	char* progid = "???";
	IDispatch* idisp = 0;
	int n;
	if (ARG(0).int_if_num(&n))
		{
		if (IUnknown* iunk = (IUnknown*) n)
			hr = iunk->QueryInterface(IID_IDispatch, (void**) &idisp);
		else
			return SuBoolean::f;
		}
	else
		{
		progid = ARG(0).str();
		// get clsid from progid
		CLSID clsid;
		USES_CONVERSION;
		hr = CLSIDFromProgID(A2OLE(progid), &clsid);
		if (FAILED(hr))
			return SuBoolean::f;
		// get idispatch
		hr = CoCreateInstance(clsid, NULL, CLSCTX_SERVER, IID_IDispatch, (void**) &idisp);
		}
	if (FAILED(hr) || ! idisp)
		return SuBoolean::f;
	return new SuCOMobject(idisp, progid);
	}
PRIM(su_COMobject, "COMobject(progid)");