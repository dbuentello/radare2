/* radare - LGPL - Copyright 2009-2015 - nibble, pancake */

#include <stdio.h>

#include <r_types.h>
#include <r_parse.h>
#include <list.h>
#include "../config.h"

R_LIB_VERSION (r_parse);

static RParsePlugin *parse_static_plugins[] =
	{ R_PARSE_STATIC_PLUGINS };

R_API RParse *r_parse_new() {
	int i;
	RParsePlugin *static_plugin;
	RParse *p = R_NEW0 (RParse);
	if (!p) return NULL;
	p->parsers = r_list_new ();
	p->parsers->free = NULL; // memleak
	p->notin_flagspace = -1;
	p->flagspace = -1;
	for (i=0; parse_static_plugins[i]; i++) {
		static_plugin = R_NEW (RParsePlugin);
		memcpy (static_plugin, parse_static_plugins[i],
			sizeof (RParsePlugin));
		r_parse_add (p, static_plugin);
	}
	return p;
}

R_API void r_parse_free(RParse *p) {
	r_list_free (p->parsers);
	free (p);
}

R_API int r_parse_add(RParse *p, RParsePlugin *foo) {
	if (foo->init)
		foo->init (p->user);
	r_list_append (p->parsers, foo);
	return true;
}

R_API int r_parse_use(RParse *p, const char *name) {
	RListIter *iter;
	RParsePlugin *h;
	r_list_foreach (p->parsers, iter, h) {
		if (!strcmp (h->name, name)) {
			p->cur = h;
			return true;
		}
	}
	return false;
}

R_API int r_parse_assemble(RParse *p, char *data, char *str) {
	char *in = strdup (str);
	int ret = false;
	char *s, *o;

	data[0]='\0';
	if (p->cur && p->cur->assemble) {
		o = data+strlen (data);
		do {
			s = strchr (str, ';');
			if (s) *s='\0';
			ret = p->cur->assemble (p, o, str);
			if (!ret) break;
			if (s) {
				str = s + 1;
				o = o+strlen (data);
				o[0]='\n';
				o[1]='\0';
				o++;
			}
		} while (s);
	}
	free (in);
	return ret;
}

R_API int r_parse_parse(RParse *p, const char *data, char *str) {
	if (p->cur && p->cur->parse)
		return p->cur->parse (p, data, str);
	return false;
}

#define isx86separator(x) ( \
	(x)==' '||(x)=='\t'||(x)=='\n'|| (x)=='\r'||(x)==' '|| \
	(x)==','||(x)==';'||(x)=='['||(x)==']'|| \
	(x)=='('||(x)==')'||(x)=='{'||(x)=='}'||(x)=='\x1b')

static bool isvalidflag(RFlagItem *flag) {
	return (flag && strchr (flag->name, '.'));
}

static char *findNextNumber(char *op) {
	bool ansi_found = false;
	char *p = op;
	if (p && *p) {
		const char *o = NULL;
		while (*p) {
			if (*p == 0x1b) {
				p++;
				if (!*p) break;
				if (*p == '[') {
					p++;
					if (p[0] && p[1] == ';') {
						// "\x1b[%d;2;%d;%d;%dm", fgbg, r, g, b
						// "\x1b[%d;5;%dm", fgbg, rgb (r, g, b)
						for (; p[0] && p[1] && p[0] != 0x1b && p[1] != '\\'; p++);
						if (p[1] == '\\') p++;
					} else {
						// "\x1b[%dm", 30 + k
						for (; *p && *p != 'J' && *p != 'm' && *p != 'H'; p++);
						if (*p) p++;
					}
					ansi_found = true;
				}
				o = p - 1;
			} else {
				bool is_space = ansi_found;
				ansi_found = false;
				if (!is_space) {
					is_space = (p != op && (*o == ' ' || *o == ',' || *o == '['));
				}
				if (is_space && *p >= '0' && *p <= '9')
					return p;
				o = p++;
			}
		}
	}
	return NULL;
}

static int filter(RParse *p, RFlag *f, char *data, char *str, int len) {
	char *ptr = data, *ptr2;
	RAnalFunction *fcn;
	RFlagItem *flag;
	ut64 off;
	int x86 = (p&&p->cur&&p->cur->name)?
		(strstr (p->cur->name, "x86")? 1: 0): 0;
	if (!data || !p) return 0;
#if FILTER_DWORD
	ptr2 = strstr (ptr, "dword ");
	if (ptr2)
		memmove (ptr2, ptr2 + 6, strlen (ptr2+  6) + 1);
#endif
	ptr2 = NULL;
	// remove "dword" 2
	while ((ptr = findNextNumber (ptr))) {
		if (x86) for (ptr2 = ptr; *ptr2 && !isx86separator (*ptr2); ptr2++);
		else for (ptr2 = ptr; *ptr2 && (*ptr2!=']' || (*ptr2=='\x1b') || !isseparator (*ptr2)); ptr2++);
		off = r_num_math (NULL, ptr);
		if (off > 0xff) {
			fcn = r_anal_get_fcn_in (p->anal, off, 0);
			if (fcn) {
				if (fcn->addr == off) {
					*ptr = 0;
					// hack to realign pointer for colours
					ptr2--;
					if (*ptr2 != 0x1b)
						ptr2++;
					snprintf (str, len, "%s%s%s", data, fcn->name,
							(ptr!=ptr2)? ptr2: "");
					return true;
				}
			}
			if (f) {
				flag = r_flag_get_i2 (f, off);
				if (!flag) {
					flag = r_flag_get_i (f, off);
				}
				if (isvalidflag (flag)) {
					if (p->notin_flagspace != -1) {
						if (p->flagspace == flag->space)
							continue;
					} else if (p->flagspace != -1 && (p->flagspace != flag->space)) {
						ptr = ptr2;
						continue;
					}
					*ptr = 0;
					// hack to realign pointer for colours
					ptr2--;
					if (*ptr2 != 0x1b)
						ptr2++;
					snprintf (str, len, "%s%s%s", data, flag->name,
							(ptr != ptr2)? ptr2: "");
					return true;
				}
			}
		}
		if (p->hint) {
			int immbase = p->hint->immbase;
			int endian = 0;
			char num[256], *pnum;
			strncpy (num, ptr, sizeof (num)-2);
			for (pnum = num; *pnum; pnum++) {
				if (IS_NUMBER (*pnum))
					continue;
				if (*pnum=='x') continue;
				break;
			}
			*pnum = 0;
			if (p->anal && p->anal->big_endian)
				endian = 1;
			switch (immbase) {
			case 0:
				// do nothing
				break;
			case 1:
				r_num_to_bits (num, off);
				strcat (num, "b");
				break;
			case 2: // hack for ascii

				memset(num, 0, sizeof(num));
				pnum = num;
				*pnum++ = '\'';

				// Convert *off* to ascii string, byte by byte.
				// Since *num* is 256 bytes long, we can omit
				// overflow checks.
				while (off) {
					unsigned char ch;

					if (endian) {
						ch = off >> (8 * (sizeof(off) - 1));
						off <<= 8;
					} else {
						ch = off & 0xff;
						off >>= 8;
					}

					//Skip first '\x00' bytes
					if (num[1] == '\0' && ch == '\0') continue;

					if (IS_PRINTABLE(ch)) {
						*pnum++ = ch;
					} else {
						int sz;

						sz = sprintf(pnum, "\\x%2.2x", ch);
						if (sz < 0) break;
						pnum += sz;
					}
				}
				*pnum++ = '\'';
				*pnum = '\0';
				break;
			case 8:
				snprintf (num, sizeof (num), "0%o", (int)off);
				break;
			case 10:
				snprintf (num, sizeof (num), "%" PFMT64d, (st64)off);
				break;
			case 16:
				/* do nothing */
			default:
				snprintf (num, sizeof (num), "0x%"PFMT64x, (ut64) off);
				break;
			}
			*ptr = 0;
			snprintf (str, len, "%s%s%s", data, num, (ptr != ptr2)? ptr2: "");
			return true;
		}
		ptr = ptr2;
	}
	strncpy (str, data, len);
	return false;
}

R_API int r_parse_filter(RParse *p, RFlag *f, char *data, char *str, int len) {
	filter (p, f, data, str, len);
	if (p->cur && p->cur->filter)
		return p->cur->filter (p, f, data, str, len);
	return false;
}

R_API bool r_parse_varsub(RParse *p, RAnalFunction *f, ut64 addr, int oplen, char *data, char *str, int len) {
	if (p->cur && p->cur->varsub) {
		return p->cur->varsub (p, f, addr, oplen, data, str, len);
	}
	return false;
}

/* setters */
R_API void r_parse_set_user_ptr(RParse *p, void *user) {
	p->user = user;
}

R_API void r_parse_set_flagspace(RParse *p, int fs) {
	p->flagspace = fs;
}

/* TODO: DEPRECATE */
R_API int r_parse_list(RParse *p) {
	RListIter *iter;
	RParsePlugin *h;
	r_list_foreach (p->parsers, iter, h) {
		printf ("parse %10s %s\n", h->name, h->desc);
	}
	return false;
}
