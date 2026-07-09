#ifndef KIWILIB_LIBINTL_H
#define KIWILIB_LIBINTL_H

char* gettext(const char* msgid);
char* dgettext(const char* domainname, const char* msgid);
char* dcgettext(const char* domainname, const char* msgid, int category);
char* ngettext(const char* msgid1, const char* msgid2, unsigned long n);
char* textdomain(const char* domainname);
char* bindtextdomain(const char* domainname, const char* dirname);

#define _(msgid) gettext(msgid)

#endif // KIWILIB_LIBINTL_H
