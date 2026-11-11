/* ===================================================================
   SMART LIBRARY MANAGEMENT SYSTEM  -  Team 2
   Pure C web application: C = web server + logic, Browser = interface.
   Data is stored in text files:  books.txt  members.txt  issues.txt
   Every record is one line with fields separated by the '|' symbol.
   Build (Windows): gcc main.c -o library.exe -lws2_32
   Build (Linux)  : gcc main.c -o library
   =================================================================== */
#ifdef _MSC_VER                     /* Visual Studio (MSVC) only: hide old-function warnings, link Windows sockets */
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32.lib")
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32                       /* Windows sockets */
#include <winsock2.h>
#else                               /* Linux / Mac sockets */
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int SOCKET;
#define closesocket close
#endif

#define PORT 8080
#define MAXB 200            /* max books   */
#define MAXM 200            /* max members */
#define MAXI 1000           /* max issue records */
#define LOAN_DAYS 30        /* due = issue date + 1 month (30 days); after this the book is overdue */
#define FINE_PER_DAY 2      /* Rs 2 per late day */
#define MAX_ISSUE 3         /* books one member can hold */

/* ---------- structures (one per record type) ---------- */
typedef struct { int id; char title[60], author[40], cat[30], pub[40]; int total, avail, year; } Book;
typedef struct { int id; char name[40], dept[30], yr[12], phone[16], email[50]; } Member;
typedef struct { int bid, mid; long long issued, due, returned; } Issue; /* returned=0 -> still out */

Book books[MAXB];  Member mem[MAXM];  Issue iss[MAXI];
int nb, nm, ni;                     /* how many records are loaded */

/* ---------- output buffer: the JSON/HTML we send back ---------- */
static char out[2400000]; int ol;
void jp(const char *f, ...) {       /* append text to the buffer (like printf) */
    va_list a; va_start(a, f);
    ol += vsnprintf(out + ol, sizeof out - ol, f, a);
    if (ol > (int)sizeof out - 1) ol = sizeof out - 1;
    va_end(a);
}
void js(const char *s) {            /* append a JSON string, escaping " and \ */
    jp("\"");
    for (; *s; s++) { if (*s == '"' || *s == '\\') jp("\\"); jp("%c", *s); }
    jp("\"");
}
void reply(int ok, const char *msg) { /* {"ok":true/false,"msg":"..."} */
    ol = 0; jp("{\"ok\":%s,\"msg\":", ok ? "true" : "false"); js(msg); jp("}");
}

/* ---------- small helper functions ---------- */
void urldecode(char *s) {           /* turn %20 and + back into normal text */
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}
/* read value of key from "a=1&b=2"; removes | and newlines; trims spaces */
void getp(const char *q, const char *key, char *dst, int n) {
    size_t kl = strlen(key); const char *p = q; dst[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1; int i = 0;
            while (*v && *v != '&' && i < n - 1) dst[i++] = *v++;
            dst[i] = 0; urldecode(dst);
            for (char *c = dst; *c; c++) if (*c == '|' || *c == '\n' || *c == '\r') *c = ' ';
            while (*dst == ' ') memmove(dst, dst + 1, strlen(dst));       /* trim left  */
            for (i = (int)strlen(dst); i > 0 && dst[i - 1] == ' '; i--) dst[i - 1] = 0; /* trim right */
            return;
        }
        p = strchr(p, '&'); if (p) p++;
    }
}
int digits(const char *s) {         /* true if s is only digits (and not empty) */
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}
int isnum(const char *s) { return strlen(s) <= 9 && digits(s); }  /* ID-style number */
int has(const char *h, const char *n) {   /* case-insensitive "contains" */
    char a[100], b[100]; int i;
    for (i = 0; h[i] && i < 99; i++) a[i] = (char)tolower((unsigned char)h[i]);
    a[i] = 0;
    for (i = 0; n[i] && i < 99; i++) b[i] = (char)tolower((unsigned char)n[i]);
    b[i] = 0;
    return strstr(a, b) != NULL;
}
void fmt(long long t, char *s) { time_t x = (time_t)t; strftime(s, 16, "%Y-%m-%d", localtime(&x)); }
int fb(int id) { for (int i = 0; i < nb; i++) if (books[i].id == id) return i; return -1; }
int fm(int id) { for (int i = 0; i < nm; i++) if (mem[i].id == id) return i; return -1; }
/* count books not yet returned; use -1 as "any" for mid or bid */
int active(int mid, int bid) {
    int c = 0;
    for (int i = 0; i < ni; i++)
        if (!iss[i].returned && (mid < 0 || iss[i].mid == mid) && (bid < 0 || iss[i].bid == bid)) c++;
    return c;
}
/* late days (a part of a day counts as a full day) */
long long late_days(const Issue *s, long long now) {
    long long end = s->returned ? s->returned : now;
    return end > s->due ? (end - s->due + 86399) / 86400 : 0;
}

/* ---------- FILE HANDLING: load and save ---------- */
void load(void) {
    FILE *f; char l[512]; nb = nm = ni = 0;
    if ((f = fopen("books.txt", "r"))) {
        while (nb < MAXB && fgets(l, sizeof l, f)) { Book *b = &books[nb];
            if (sscanf(l, "%d|%59[^|]|%39[^|]|%29[^|]|%39[^|]|%d|%d|%d", &b->id, b->title, b->author, b->cat, b->pub, &b->year, &b->total, &b->avail) == 8) nb++; }
        fclose(f);
    }
    if ((f = fopen("members.txt", "r"))) {
        while (nm < MAXM && fgets(l, sizeof l, f)) { Member *m = &mem[nm];
            if (sscanf(l, "%d|%39[^|]|%29[^|]|%11[^|]|%15[^|]|%49[^|\n]", &m->id, m->name, m->dept, m->yr, m->phone, m->email) == 6) nm++; }
        fclose(f);
    }
    if ((f = fopen("issues.txt", "r"))) {
        while (ni < MAXI && fgets(l, sizeof l, f)) { Issue *s = &iss[ni];
            if (sscanf(l, "%d|%d|%lld|%lld|%lld", &s->bid, &s->mid, &s->issued, &s->due, &s->returned) == 5) ni++; }
        fclose(f);
    }
}
void save(void) {
    FILE *f; int i;
    if ((f = fopen("books.txt", "w"))) { for (i = 0; i < nb; i++) fprintf(f, "%d|%s|%s|%s|%s|%d|%d|%d\n",
        books[i].id, books[i].title, books[i].author, books[i].cat, books[i].pub, books[i].year, books[i].total, books[i].avail); fclose(f); }
    if ((f = fopen("members.txt", "w"))) { for (i = 0; i < nm; i++) fprintf(f, "%d|%s|%s|%s|%s|%s\n",
        mem[i].id, mem[i].name, mem[i].dept, mem[i].yr, mem[i].phone, mem[i].email); fclose(f); }
    if ((f = fopen("issues.txt", "w"))) { for (i = 0; i < ni; i++) fprintf(f, "%d|%d|%lld|%lld|%lld\n",
        iss[i].bid, iss[i].mid, iss[i].issued, iss[i].due, iss[i].returned); fclose(f); }
}

/* ---------- EXCEL EXPORT: builds a real .xlsx file using only C ---------- */
/* An .xlsx file is really a ZIP of small XML files. We write the XML text ourselves
   and pack it into a ZIP using the "stored" method (no compression), so no library is needed. */
#define XMLH "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
#define NS "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
#define REL "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define PARTMAX 500000
static char part[8][PARTMAX]; static int plen[8], cur;          /* the 8 files inside the .xlsx */
static const char *ctype = "application/json", *extra_hdr = ""; /* reply headers set by route() */
static const char *pname[8] = { "[Content_Types].xml", "_rels/.rels", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
    "xl/styles.xml", "xl/worksheets/sheet1.xml", "xl/worksheets/sheet2.xml", "xl/worksheets/sheet3.xml" };

void xp(const char *f, ...) {                 /* append text to the current XML file */
    va_list a; va_start(a, f);
    plen[cur] += vsnprintf(part[cur] + plen[cur], PARTMAX - plen[cur], f, a);
    if (plen[cur] > PARTMAX - 1) plen[cur] = PARTMAX - 1;
    va_end(a);
}
void xe(const char *s) {                      /* write text safely inside XML (& < >) */
    for (; *s; s++) { if (*s == '&') xp("&amp;"); else if (*s == '<') xp("&lt;"); else if (*s == '>') xp("&gt;"); else xp("%c", *s); }
}
void cs(const char *s, int head) { xp("<c t=\"inlineStr\"%s><is><t>", head ? " s=\"1\"" : ""); xe(s); xp("</t></is></c>"); } /* text cell */
void cn(long long v) { xp("<c><v>%lld</v></c>", v); }                                                                    /* number cell */
void sheet_open(int c, const char *heads) {   /* start a sheet and write its bold header row */
    char t[300], *p; cur = c; plen[c] = 0; strcpy(t, heads);
    xp(XMLH "<worksheet " NS "><cols><col min=\"1\" max=\"10\" width=\"22\" customWidth=\"1\"/></cols><sheetData><row>");
    for (p = strtok(t, ","); p; p = strtok(NULL, ",")) cs(p, 1);
    xp("</row>");
}
unsigned zcrc(const unsigned char *p, int n) {   /* CRC-32 checksum that every ZIP file needs */
    unsigned c = ~0u;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
void w16(unsigned v) { out[ol++] = (char)(v & 255); out[ol++] = (char)((v >> 8) & 255); }   /* write little-endian numbers */
void w32(unsigned v) { w16(v & 0xFFFF); w16(v >> 16); }

void make_xlsx(void) {
    int i, k; long long now = (long long)time(NULL); char d1[16], d2[16], d3[16];
    /* 1) the fixed files */
    cur = 0; plen[0] = 0;
    xp(XMLH "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/><Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    for (k = 1; k <= 3; k++) xp("<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", k);
    xp("</Types>");
    cur = 1; plen[1] = 0;
    xp(XMLH "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"" REL "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>");
    cur = 2; plen[2] = 0;
    xp(XMLH "<workbook " NS " xmlns:r=\"" REL "\"><sheets><sheet name=\"Books\" sheetId=\"1\" r:id=\"rId1\"/><sheet name=\"Members\" sheetId=\"2\" r:id=\"rId2\"/><sheet name=\"Issues\" sheetId=\"3\" r:id=\"rId3\"/></sheets></workbook>");
    cur = 3; plen[3] = 0;
    xp(XMLH "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (k = 1; k <= 3; k++) xp("<Relationship Id=\"rId%d\" Type=\"" REL "/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", k, k);
    xp("<Relationship Id=\"rId4\" Type=\"" REL "/styles\" Target=\"styles.xml\"/></Relationships>");
    cur = 4; plen[4] = 0;      /* styles: style 0 = normal, style 1 = bold (for headings) */
    xp(XMLH "<styleSheet " NS "><fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Arial\"/></font><font><b/><sz val=\"11\"/><name val=\"Arial\"/></font></fonts><fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills><borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs><cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/><xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");

    /* 2) the three sheets: Books, Members, Issues */
    sheet_open(5, "Book ID,Title,Author,Category,Publisher,Year,Total Copies,Available Copies");
    for (i = 0; i < nb; i++) { Book *b = &books[i]; xp("<row>"); cn(b->id); cs(b->title, 0); cs(b->author, 0); cs(b->cat, 0); cs(b->pub, 0); cn(b->year); cn(b->total); cn(b->avail); xp("</row>"); }
    xp("</sheetData></worksheet>");
    sheet_open(6, "Member ID,Name,Department,Year of Study,Phone,Email,Books Held");
    for (i = 0; i < nm; i++) { Member *m = &mem[i]; xp("<row>"); cn(m->id); cs(m->name, 0); cs(m->dept, 0); cs(m->yr, 0); cs(m->phone, 0); cs(m->email, 0); cn(active(m->id, -1)); xp("</row>"); }
    xp("</sheetData></worksheet>");
    sheet_open(7, "Member ID,Member Name,Book ID,Book Title,Issue Date,Due Date,Return Date,Status,Days Late,Fine (Rs)");
    for (i = 0; i < ni; i++) { Issue *s = &iss[i]; long long od = late_days(s, now);
        int mi = fm(s->mid), bi = fb(s->bid);
        fmt(s->issued, d1); fmt(s->due, d2); if (s->returned) fmt(s->returned, d3);
        xp("<row>"); cn(s->mid); cs(mi >= 0 ? mem[mi].name : "(deleted)", 0); cn(s->bid); cs(bi >= 0 ? books[bi].title : "(deleted)", 0);
        cs(d1, 0); cs(d2, 0); cs(s->returned ? d3 : "-", 0); cs(s->returned ? "Returned" : (od ? "Overdue" : "Issued"), 0);
        cn(od); cn(od * FINE_PER_DAY); xp("</row>"); }
    xp("</sheetData></worksheet>");

    /* 3) pack the 8 files into a ZIP (local headers + data, then the central directory) */
    unsigned crc[8], off[8], cd; ol = 0;
    for (i = 0; i < 8; i++) {
        int n = (int)strlen(pname[i]); crc[i] = zcrc((unsigned char *)part[i], plen[i]); off[i] = ol;
        w32(0x04034b50); w16(20); w16(0); w16(0); w16(0); w16(0x5A21); w32(crc[i]); w32(plen[i]); w32(plen[i]); w16(n); w16(0);
        memcpy(out + ol, pname[i], n); ol += n; memcpy(out + ol, part[i], plen[i]); ol += plen[i];
    }
    cd = ol;
    for (i = 0; i < 8; i++) {
        int n = (int)strlen(pname[i]);
        w32(0x02014b50); w16(20); w16(20); w16(0); w16(0); w16(0); w16(0x5A21); w32(crc[i]); w32(plen[i]); w32(plen[i]);
        w16(n); w16(0); w16(0); w16(0); w16(0); w32(0); w32(off[i]); memcpy(out + ol, pname[i], n); ol += n;
    }
    { unsigned cdsz = ol - cd;   /* size of the central directory, measured BEFORE writing the end record */
      w32(0x06054b50); w16(0); w16(0); w16(8); w16(8); w32(cdsz); w32(cd); w16(0); }
}

/* ---------- ROUTER: decides what to do for each URL ---------- */
void route(const char *p, const char *q) {
    char a[64], b[64], c[64], d[64], e[64], f[64], g[64], d1[16], d2[16];
    long long now = (long long)time(NULL); int i, k;
    ol = 0;

    /* ===== BOOKS ===== */
    if (!strcmp(p, "/api/books")) {                         /* list + search */
        getp(q, "q", a, 64); jp("["); k = 0;
        for (i = 0; i < nb; i++) { Book *x = &books[i]; char ids[16]; sprintf(ids, "%d", x->id);
            if (!(has(x->title, a) || has(x->author, a) || !strcmp(ids, a))) continue;
            jp("%s{\"id\":%d,\"title\":", k++ ? "," : "", x->id); js(x->title);
            jp(",\"author\":"); js(x->author); jp(",\"cat\":"); js(x->cat);
            jp(",\"pub\":"); js(x->pub); jp(",\"year\":%d,\"total\":%d,\"avail\":%d}", x->year, x->total, x->avail);
        }
        jp("]");
    }
    else if (!strcmp(p, "/api/book/add") || !strcmp(p, "/api/book/update")) {
        int upd = !strcmp(p, "/api/book/update");
        getp(q, "id", a, 64); getp(q, "title", b, 60); getp(q, "author", c, 40);
        getp(q, "cat", d, 30); getp(q, "total", e, 64); getp(q, "pub", f, 40); getp(q, "year", g, 64);
        if (!isnum(a) || !*b || !*c || !*d || !isnum(e) || atoi(e) < 1 ||
            !*f || !isnum(g) || atoi(g) < 1000 || atoi(g) > 2100) {   /* validation */
            reply(0, "Fill every field. ID and copies must be positive numbers; year must be 4 digits."); return; }
        i = fb(atoi(a));
        if (!upd) {
            if (i >= 0) { reply(0, "Duplicate Book ID. Use a new ID."); return; }
            if (nb >= MAXB) { reply(0, "Book list is full."); return; }
            Book *x = &books[nb++]; x->id = atoi(a); strcpy(x->title, b); strcpy(x->author, c);
            strcpy(x->cat, d); strcpy(x->pub, f); x->year = atoi(g); x->total = x->avail = atoi(e); save(); reply(1, "Book added.");
        } else {
            if (i < 0) { reply(0, "Book ID not found."); return; }
            int av = books[i].avail + atoi(e) - books[i].total;   /* keep issued count correct */
            if (av < 0) { reply(0, "Total copies cannot be less than copies already issued."); return; }
            strcpy(books[i].title, b); strcpy(books[i].author, c); strcpy(books[i].cat, d);
            strcpy(books[i].pub, f); books[i].year = atoi(g);
            books[i].total = atoi(e); books[i].avail = av; save(); reply(1, "Book updated.");
        }
    }
    else if (!strcmp(p, "/api/book/delete")) {
        getp(q, "id", a, 64); i = isnum(a) ? fb(atoi(a)) : -1;
        if (i < 0) reply(0, "Book not found.");
        else if (active(-1, books[i].id)) reply(0, "Cannot delete: this book is currently issued.");
        else { memmove(&books[i], &books[i + 1], (nb - i - 1) * sizeof(Book)); nb--; save(); reply(1, "Book deleted."); }
    }

    /* ===== MEMBERS ===== */
    else if (!strcmp(p, "/api/members")) {
        getp(q, "q", a, 64); jp("["); k = 0;
        for (i = 0; i < nm; i++) { Member *x = &mem[i]; char ids[16]; sprintf(ids, "%d", x->id);
            if (!(has(x->name, a) || has(x->dept, a) || !strcmp(ids, a))) continue;
            jp("%s{\"id\":%d,\"name\":", k++ ? "," : "", x->id); js(x->name);
            jp(",\"dept\":"); js(x->dept); jp(",\"yr\":"); js(x->yr); jp(",\"phone\":"); js(x->phone);
            jp(",\"email\":"); js(x->email); jp(",\"holding\":%d}", active(x->id, -1));
        }
        jp("]");
    }
    else if (!strcmp(p, "/api/member/add") || !strcmp(p, "/api/member/update")) {
        int upd = !strcmp(p, "/api/member/update");
        getp(q, "id", a, 64); getp(q, "name", b, 40); getp(q, "dept", c, 30);
        getp(q, "phone", d, 16); getp(q, "email", e, 50); getp(q, "yr", f, 12);
        char *at = strchr(e, '@');
        if (!isnum(a) || !*b || !*c || !*f) { reply(0, "ID, name, department and year are required. ID must be a number."); return; }
        if (strlen(d) != 10 || !digits(d)) { reply(0, "Phone must be exactly 10 digits."); return; }
        if (!at || at == e || !strchr(at, '.')) { reply(0, "Enter a valid email, like name@college.edu."); return; }
        i = fm(atoi(a));
        if (!upd) {
            if (i >= 0) { reply(0, "Duplicate Member ID. Use a new ID."); return; }
            if (nm >= MAXM) { reply(0, "Member list is full."); return; }
            i = nm++; mem[i].id = atoi(a);
        } else if (i < 0) { reply(0, "Member ID not found."); return; }
        strcpy(mem[i].name, b); strcpy(mem[i].dept, c); strcpy(mem[i].yr, f); strcpy(mem[i].phone, d); strcpy(mem[i].email, e);
        save(); reply(1, upd ? "Member updated." : "Member added.");
    }
    else if (!strcmp(p, "/api/member/delete")) {
        getp(q, "id", a, 64); i = isnum(a) ? fm(atoi(a)) : -1;
        if (i < 0) reply(0, "Member not found.");
        else if (active(mem[i].id, -1)) reply(0, "Cannot delete: member still holds issued books.");
        else { memmove(&mem[i], &mem[i + 1], (nm - i - 1) * sizeof(Member)); nm--; save(); reply(1, "Member deleted."); }
    }

    /* ===== ISSUE BOOK ===== */
    else if (!strcmp(p, "/api/issue")) {
        getp(q, "member", a, 64); getp(q, "book", b, 64); getp(q, "demo", c, 64);
        int m = isnum(a) ? fm(atoi(a)) : -1, bk = isnum(b) ? fb(atoi(b)) : -1;
        if (m < 0) reply(0, "Member ID not found.");
        else if (bk < 0) reply(0, "Book ID not found.");
        else if (books[bk].avail < 1) reply(0, "No copies available right now.");
        else if (active(mem[m].id, -1) >= MAX_ISSUE) reply(0, "This member already holds 3 books (the limit).");
        else if (active(mem[m].id, books[bk].id)) reply(0, "This member already has this book.");
        else if (ni >= MAXI) reply(0, "Issue records are full.");
        else {
            Issue *s = &iss[ni++]; s->mid = mem[m].id; s->bid = books[bk].id;
            s->issued = now - (isnum(c) ? atoi(c) : 0) * 86400LL;  /* "demo" back-dates the issue to test fines */
            s->due = s->issued + LOAN_DAYS * 86400LL; s->returned = 0;
            books[bk].avail--; save(); fmt(s->due, d1);
            sprintf(f, "Book issued. Due date: %s", d1); reply(1, f);
        }
    }

    /* ===== RETURN BOOK ===== */
    else if (!strcmp(p, "/api/return")) {
        getp(q, "member", a, 64); getp(q, "book", b, 64);
        for (i = 0; i < ni; i++) if (!iss[i].returned && iss[i].mid == atoi(a) && iss[i].bid == atoi(b)) break;
        if (!isnum(a) || !isnum(b) || i >= ni) { reply(0, "No active issue found for this member and book."); return; }
        iss[i].returned = now; books[fb(iss[i].bid)].avail++; save();
        sprintf(f, "Book returned. Fine: Rs %lld", late_days(&iss[i], now) * FINE_PER_DAY); reply(1, f);
    }

    /* ===== ISSUED LIST (overdue=1 shows only late books) ===== */
    else if (!strcmp(p, "/api/issues")) {
        getp(q, "overdue", a, 64); jp("["); k = 0;
        for (i = 0; i < ni; i++) { Issue *s = &iss[i]; long long od = late_days(s, now);
            int bi = fb(s->bid), mi = fm(s->mid);
            if (s->returned || bi < 0 || mi < 0 || (*a == '1' && !od)) continue;
            fmt(s->issued, d1); fmt(s->due, d2);
            jp("%s{\"mid\":%d,\"member\":", k++ ? "," : "", s->mid); js(mem[mi].name);
            jp(",\"bid\":%d,\"book\":", s->bid); js(books[bi].title);
            jp(",\"issued\":\"%s\",\"due\":\"%s\",\"days\":%lld,\"fine\":%lld}", d1, d2, od, od * FINE_PER_DAY);
        }
        jp("]");
    }

    /* ===== DASHBOARD ===== */
    else if (!strcmp(p, "/api/dashboard")) {
        int over = 0; long long fine = 0;
        for (i = 0; i < ni; i++) if (!iss[i].returned && late_days(&iss[i], now)) { over++; fine += late_days(&iss[i], now) * FINE_PER_DAY; }
        jp("{\"books\":%d,\"members\":%d,\"issued\":%d,\"overdue\":%d,\"fine\":%lld}", nb, nm, active(-1, -1), over, fine);
    }

    /* ===== SEARCH SUGGESTIONS (up to 5 titles/authors) ===== */
    else if (!strcmp(p, "/api/suggest")) {
        getp(q, "q", a, 64); jp("["); k = 0;
        for (i = 0; *a && i < nb && k < 5; i++) {
            if (has(books[i].title, a)) { if (k++) jp(","); js(books[i].title); }
            else if (has(books[i].author, a)) { if (k++) jp(","); js(books[i].author); }
        }
        jp("]");
    }
    /* ===== DOWNLOAD ALL DATA AS EXCEL (.xlsx) ===== */
    else if (!strcmp(p, "/api/export")) {
        make_xlsx();
        ctype = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
        extra_hdr = "Content-Disposition: attachment; filename=\"library_data.xlsx\"\r\n";
    }
    else reply(0, "Unknown request.");
}

/* ---------- send an HTTP response ---------- */
void respond(SOCKET c, const char *status, const char *type) {
    char h[400]; int n = sprintf(h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n", status, type, ol, extra_hdr);
    send(c, h, n, 0);
    for (int sent = 0, r; sent < ol; sent += r) { r = send(c, out + sent, ol - sent, 0); if (r <= 0) break; }
}

/* ---------- MAIN: create socket, wait for browser requests ---------- */
int main(void) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    load();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0); int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    struct sockaddr_in ad; memset(&ad, 0, sizeof ad);
    ad.sin_family = AF_INET; ad.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ad.sin_port = htons(PORT);
    if (bind(s, (struct sockaddr *)&ad, sizeof ad) < 0 || listen(s, 5) < 0) { printf("Port %d is busy. Close the other program.\n", PORT); return 1; }
    printf("Library server running. Open http://localhost:%d in your browser.\n", PORT);
    for (;;) {
        SOCKET c = accept(s, NULL, NULL); char req[4096], path[512], *q;
        int n = recv(c, req, sizeof req - 1, 0);
        if (n <= 0 || sscanf((req[n] = 0, req), "%*s %511s", path) != 1) { closesocket(c); continue; }
        q = strchr(path, '?'); if (q) *q++ = 0; else q = "";
        if (!strcmp(path, "/")) {                           /* serve the web page */
            FILE *f = fopen("index.html", "rb");
            if (f) { ol = (int)fread(out, 1, sizeof out - 1, f); fclose(f); respond(c, "200 OK", "text/html; charset=utf-8"); }
            else { ol = 0; jp("index.html not found. Keep it next to the program."); respond(c, "404 Not Found", "text/plain"); }
        } else if (!strncmp(path, "/api/", 5)) { ctype = "application/json"; extra_hdr = ""; route(path, q); respond(c, "200 OK", ctype); }
        else { ol = 0; jp("Not found"); respond(c, "404 Not Found", "text/plain"); }
        closesocket(c);
    }
}
