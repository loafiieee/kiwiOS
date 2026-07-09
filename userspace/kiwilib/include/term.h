#ifndef KIWILIB_TERM_H
#define KIWILIB_TERM_H

int setupterm(char* term, int fd, int* errret);
char* tigetstr(const char* capname);
int tigetnum(const char* capname);
int tigetflag(const char* capname);
char* tparm(const char* str, ...);
int putp(const char* str);
int tputs(const char* str, int affcnt, int (*putc_fn)(int));

extern char PC;
extern char* UP;
extern char* BC;
extern short ospeed;

int tgetent(char* bp, const char* name);
int tgetflag(const char* id);
int tgetnum(const char* id);
char* tgetstr(const char* id, char** area);
char* tgoto(const char* cap, int col, int row);

#endif // KIWILIB_TERM_H
