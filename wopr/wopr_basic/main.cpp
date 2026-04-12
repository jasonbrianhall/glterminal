/*
 * main.c — Main interpreter loop, SIGINT handler, interactive REPL,
 *           and program entry point.
 */
#include "basic.h"

#ifdef USE_SDL_WINDOW
#include "basic_gfx.h"
#include "basic_gfx_sdl.h"
#include "display.h"
#include "basic.h"       // g_break, BASIC_NS
#include "DejaVuMono.h"  // DEJAVU_REGULAR_FONT_B64 / _SIZE
#endif


#ifdef _WIN32
#include <windows.h>
#include <ctype.h>

static char *strcasestr(char *haystack, char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}
#endif

#ifdef INLINEBASIC
#include "inline_basic.h"
#endif


#include "basic_print.h"
#define printf(...) basic_printf(__VA_ARGS__)

/*
 * C-linkage storage — defined before the namespace opens.
 * BASIC_AUTOLOAD_SYM / BASIC_BREAK_SYM expand to the unique C symbol name
 * for this build variant (e.g. g_autoload_path / g_break for WOPR,
 * fb_autoload_path / fb_g_break for FELIX_BASIC).
 * The host binary references these by their plain C names directly.
 */
#if defined(WOPR) || defined(FELIX_BASIC)
    char                    BASIC_AUTOLOAD_SYM[DEFAULT_BUFFER] = {0};
    volatile sig_atomic_t   BASIC_BREAK_SYM         = 0;
#endif

BASIC_NS_BEGIN

#if defined(WOPR) || defined(FELIX_BASIC)
/* g_autoload_path is a reference into the C-linkage buffer so host and
 * interpreter share the same storage.  g_break is handled by the macro
 * in basic_ns.h — no namespace-level variable needed. */
char (&g_autoload_path)[DEFAULT_BUFFER] = ::BASIC_AUTOLOAD_SYM;
#endif


/* ================================================================
 * SIGINT handler — sets g_break so the run loop can stop cleanly
 * ================================================================ */
static void sigint_handler(int sig) {
    (void)sig;
    g_break = 1;
}

static void install_sigint(void) {
#ifndef _WIN32
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#else
    signal(SIGINT, sigint_handler);
#endif
}

/* ================================================================
 * Interpreter run loop
 * ================================================================ */
void run(void) { run_from(0); }

void run_from(int start_pc) {
    g_break = 0;
    Interp ip = { .pc = start_pc, .running = 1 };
    sound_init();


#ifdef USE_SDL_WINDOW
    // ------------------------------------------------------------
    // SDL WINDOW BUILD — real 60 FPS render loop
    // ------------------------------------------------------------
    Uint32 last_frame = SDL_GetTicks();

    while (ip.running && ip.pc < g_nlines && !g_break) {

        // Pump SDL events every iteration
        if (!gfx_sdl_pump())
            break;
        gfx_maybe_mark_dirty();
        // Render at ~60 FPS
        Uint32 now = SDL_GetTicks();
        if (now - last_frame >= 16) {
            gfx_sdl_render();
            last_frame = now;
        }

        // Execute one BASIC instruction
        int old_pc = ip.pc;
        int jumped = dispatch(&ip, g_lines[ip.pc].text);

        if (jumped < 0 && g_error_handler[0]) {
            g_error_resume_pc = old_pc;
            cmd_goto(&ip, g_error_handler);
            jumped = 1;
        } else if (!jumped) {
            ip.pc = old_pc + 1;
        }
    }

    // Final frame
    gfx_sdl_render();

#else
    // ------------------------------------------------------------
    // COMMAND-LINE BUILD — original behavior
    // ------------------------------------------------------------
    while (ip.running && ip.pc < g_nlines && !g_break) {
        int old_pc = ip.pc;
        int jumped = dispatch(&ip, g_lines[ip.pc].text);

        if (jumped < 0 && g_error_handler[0]) {
            g_error_resume_pc = old_pc;
            cmd_goto(&ip, g_error_handler);
            jumped = 1;
        } else if (!jumped) {
            ip.pc = old_pc + 1;
        }
    }
#endif

    // Shared break handling
    if (g_break) {
        g_cont_pc = ip.pc;
        display_newline();
        display_print("Break\n");
        g_break = 0;
    }
}



/* ================================================================
 * main — direct-run or interactive REPL
 * ================================================================ */
#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void) {
#else
int basic_main(int argc, char **argv) {
#endif
    g_prec = DEFAULT_PREC;
    mpf_set_default_prec(g_prec);
    clear_program();   /* flush g_nlines/g_nvar/etc. left over from a prior session */
    srand((unsigned)time(NULL));
    display_init();
    install_sigint();

    /* Direct run mode: ./basic program.bas [precision_bits] */
#ifndef INLINEBASIC
#if !defined(WOPR) && !defined(FELIX_BASIC)
    if (argc >= 2) {
        if (argc >= 3) g_prec = (mp_bitcnt_t)atoi(argv[2]);
        load(argv[1]);
        prescan_data();
        run();
        display_shutdown();
        return 0;
    }
#endif
#else
char tmpfile_path[DEFAULT_BUFFER];

#ifdef _WIN32
    // Windows: use GetTempPath + GetTempFileName
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    GetTempFileNameA(tmpdir, "WOP", 0, tmpfile_path);

    FILE *dbfile = fopen(tmpfile_path, "wb");
    if (!dbfile) {
        basic_stderr("Could not open temp file\n");
        return 1;
    }

    fwrite(wopr_basic_program, 1, wopr_basic_program_len, dbfile);
    fclose(dbfile);

#else
    // POSIX: use mkstemp
    strcpy(tmpfile_path, "/tmp/woprXXXXXX");
    int fd = mkstemp(tmpfile_path);
    if (fd == -1) {
        perror("mkstemp");
        return 1;
    }

    write(fd, wopr_basic_program, wopr_basic_program_len);
    close(fd);
#endif

// NOW replace argv[1] with the filename
argv[1] = tmpfile_path;

load(argv[1]);
prescan_data();
run();
display_shutdown();
return 0;
#endif

#if defined(WOPR) || defined(FELIX_BASIC)
    /* Auto-load a file if one was set before the thread started */
    if (g_autoload_path[0]) {
        load_program(g_autoload_path);
        prescan_data();
        run();
        display_shutdown();
        return 0;
    }
#endif

    /* ----------------------------------------------------------------
     * REPL helper macros
     * ---------------------------------------------------------------- */
    #define PARSE_FILENAME(dst, src) do { \
        char *_q = sk(src); \
        if (*_q == '"') _q++; \
        int _i = 0; \
        while (*_q && *_q != '"' && *_q != ',' && !isspace((unsigned char)*_q) && _i < 255) \
            (dst)[_i++] = *_q++; \
        (dst)[_i] = '\0'; \
        if (*_q == '"') _q++; \
        (src) = _q; \
    } while(0)

    #define BAS_EXT(dst, src) do { \
        if (strchr((src), '.')) snprintf((dst), sizeof(dst), "%s", (src)); \
        else snprintf((dst), sizeof(dst), "%s.bas", (src)); \
    } while(0)

    /* ----------------------------------------------------------------
     * Interactive REPL
     * ---------------------------------------------------------------- */
    display_cls();
    display_print("WOPR BASIC\n");
    display_print("Type NEW, LOAD, RUN, LIST, FILES, HELP, or SYSTEM to exit.\n\n");

    char line[DEFAULT_BUFFER];
    for (;;) {
        display_print("Ok\n");
        display_cursor(1);
        display_getline(line, sizeof line);
        display_newline();

        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        /* SYSTEM / EXIT / QUIT / BYE */
        if (strncasecmp(p,"SYSTEM",6)==0 || strncasecmp(p,"EXIT",4)==0 ||
            strncasecmp(p,"QUIT",4)==0   || strncasecmp(p,"BYE",3)==0) {
            break;

        } else if (strncasecmp(p,"NEW",3)==0 && !isalnum((unsigned char)p[3])) {
            clear_program();
            display_print("Ok\n");

        } else if (strncasecmp(p,"FILES",5)==0 || strncasecmp(p,"DIR",3)==0) {
            char *pat = sk(p + (strncasecmp(p,"DIR",3)==0 ? 3 : 5));
            char filter[64] = ".bas";
            if (*pat == '"') pat++;
            if (*pat && *pat != '"') { strncpy(filter, pat, 63); char *eq=strchr(filter,'"'); if(eq)*eq='\0'; }
            { char cwd[DEFAULT_BUFFER]; if (getcwd(cwd, sizeof cwd)) { char hdr[DEFAULT_BUFFER+16]; snprintf(hdr, sizeof hdr, "  Directory: %s\n\n", cwd); display_print(hdr); } }
            DIR *d = opendir(".");
            if (!d) { perror("opendir"); continue; }
            struct dirent *de; int count = 0;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                char *nm = de->d_name;
                struct stat st; bool is_dir = false;
                if (stat(nm, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
                if (is_dir) {
                    char entry[68]; snprintf(entry, sizeof entry, "  %-19s/\n", nm);
                    display_print(entry); count++;
                } else {
                    size_t nl = strlen(nm), fl = strlen(filter);
                    if (fl && (nl < fl || strcasecmp(nm + nl - fl, filter) != 0)) continue;
                    char entry[68]; snprintf(entry, sizeof entry, "  %-20s\n", nm);
                    display_print(entry); count++;
                }
            }
            closedir(d);
            if (!count) display_print("  (no matching files)\n");

        } else if (strncasecmp(p,"KILL",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4;
            char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            char path[DEFAULT_BUFFER]; BAS_EXT(path, name);
            if (remove(path) == 0) printf("Deleted %s\n", path); else perror(path);

        } else if (strncasecmp(p,"RENAME",6)==0 && !isalnum((unsigned char)p[6])) {
            p += 6;
            char old[DEFAULT_BUFFER], neo[DEFAULT_BUFFER];
            PARSE_FILENAME(old, p); p = sk(p); if (*p==',') p++;
            PARSE_FILENAME(neo, p);
            char op[DEFAULT_BUFFER], np[DEFAULT_BUFFER]; BAS_EXT(op,old); BAS_EXT(np,neo);
            if (rename(op, np) == 0) printf("Renamed %s -> %s\n", op, np); else perror(op);

        } else if (strncasecmp(p,"CHDIR",5)==0 || strncasecmp(p,"CD",2)==0) {
            p += strncasecmp(p,"CHDIR",5)==0 ? 5 : 2;
            char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            if (!*name) { char cwd[DEFAULT_BUFFER]; if(getcwd(cwd,sizeof cwd)) printf("%s\n",cwd); }
            else if (chdir(name)!=0) perror(name);
            else { char cwd[DEFAULT_BUFFER]; if(getcwd(cwd,sizeof cwd)) printf("%s\n",cwd); }

        } else if (strncasecmp(p,"MKDIR",5)==0 && !isalnum((unsigned char)p[5])) {
            p += 5; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            if (
#ifdef _WIN32
                mkdir(name)
#else
                mkdir(name, 0755)
#endif
                != 0) perror(name); else printf("Created %s\n", name);

        } else if (strncasecmp(p,"RMDIR",5)==0 && !isalnum((unsigned char)p[5])) {
            p += 5; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            if (rmdir(name)!=0) perror(name); else printf("Removed %s\n", name);

        } else if (strncasecmp(p,"LOAD",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            if (!*name) { display_print("Usage: LOAD \"filename\"\n"); continue; }
            load_program(name);

        } else if (strncasecmp(p,"SAVE",4)==0 && !isalnum((unsigned char)p[4])) {
            p += 4; char name[DEFAULT_BUFFER]; PARSE_FILENAME(name, p);
            if (!*name) { display_print("Usage: SAVE \"filename\"\n"); continue; }
            save_program(name);

        } else if (strncasecmp(p,"RUN",3)==0 && !isalnum((unsigned char)p[3])) {
            if (g_nlines == 0) { display_print("No program loaded.\n"); continue; }
            g_nvar = 0; g_ctrl_top = 0; g_data_pos = 0;
            run();

        } else if (strncasecmp(p,"CONT",4)==0 && !isalnum((unsigned char)p[4])) {
            if (g_cont_pc < 0) { display_print("Can't continue\n"); continue; }
            run_from(g_cont_pc);

        } else if (strncasecmp(p,"LIST",4)==0 && !isalnum((unsigned char)p[4])) {
            p = sk(p + 4);
            int from = 0, to = 999999;
            if (isdigit((unsigned char)*p)) {
                from = atoi(p);
                while (isdigit((unsigned char)*p)) p++;
                to = 999999; p = sk(p);
                if (*p == '-') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
                else if (*p == ',') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
                else { to = 999999; }
            }
            int prev = -1;
            for (int i = 0; i < g_nlines; i++) {
                int ln = g_lines[i].linenum;
                if (ln < from || ln > to) continue;
                if (ln != prev) { if (prev != -1) display_print("\n"); char hdr[32]; snprintf(hdr,sizeof hdr,"%d ",ln); display_print(hdr); prev = ln; }
                else display_print(": ");
                display_print(g_lines[i].text);
            }
            if (prev != -1) display_print("\n");

        } else if (strncasecmp(p,"DELETE",6)==0 && !isalnum((unsigned char)p[6])) {
            p = sk(p+6);
            if (!isdigit((unsigned char)*p)) { display_print("Usage: DELETE start[-end]\n"); continue; }
            int from = atoi(p); while (isdigit((unsigned char)*p)) p++;
            int to = from; p = sk(p);
            if (*p == '-') { p=sk(p+1); to=isdigit((unsigned char)*p)?atoi(p):999999; }
            int w = 0;
            for (int i = 0; i < g_nlines; i++)
                if (g_lines[i].linenum < from || g_lines[i].linenum > to) g_lines[w++] = g_lines[i];
            printf("Deleted %d entries.\n", g_nlines - w);
            g_nlines = w;

        } else if (strncasecmp(p,"RENUM",5)==0 && !isalnum((unsigned char)p[5])) {
            p = sk(p+5);
            int new_start=10, step=10;
            if (isdigit((unsigned char)*p)) { new_start=atoi(p); while(isdigit((unsigned char)*p))p++; }
            p=sk(p); if(*p==',') p=sk(p+1);
            if (isdigit((unsigned char)*p)) { while(isdigit((unsigned char)*p))p++; }
            p=sk(p); if(*p==',') p=sk(p+1);
            if (isdigit((unsigned char)*p)) { step=atoi(p); }
            if (step < 1) step = 10;
            int old_nums[MAX_LINES], new_nums[MAX_LINES], nmap = 0, prev2 = -1;
            for (int i = 0; i < g_nlines; i++) {
                if (g_lines[i].linenum != prev2) {
                    old_nums[nmap] = g_lines[i].linenum;
                    new_nums[nmap] = new_start + nmap * step;
                    nmap++; prev2 = g_lines[i].linenum;
                }
            }
            for (int i = 0; i < g_nlines; i++) {
                for (int j = 0; j < nmap; j++) {
                    if (g_lines[i].linenum == old_nums[j]) { g_lines[i].linenum = new_nums[j]; break; }
                }
                char *kws[] = {"GOTO","GOSUB","THEN","ELSE","RESTORE","RUN", NULL};
                for (int k = 0; kws[k]; k++) {
                    char *kw = strcasestr(g_lines[i].text, kws[k]);
                    if (!kw) continue;
                    char *np2 = kw + strlen(kws[k]);
                    while (isspace((unsigned char)*np2)) np2++;
                    if (!isdigit((unsigned char)*np2)) continue;
                    int old_target = atoi(np2);
                    for (int j = 0; j < nmap; j++) {
                        if (old_nums[j] == old_target) {
                            char newnum[16]; snprintf(newnum,sizeof newnum,"%d",new_nums[j]);
                            char after_num[MAX_LINE_LEN];
                            char *end_num = np2;
                            while (isdigit((unsigned char)*end_num)) end_num++;
                            strncpy(after_num, end_num, MAX_LINE_LEN-1);
                            snprintf(np2, MAX_LINE_LEN-(np2-g_lines[i].text), "%s%s", newnum, after_num);
                            break;
                        }
                    }
                }
            }
            qsort(g_lines, g_nlines, sizeof(Line), line_cmp);
            printf("Renumbered %d logical lines starting at %d step %d.\n", nmap, new_start, step);

        } else if (strncasecmp(p,"HELP",4)==0) {
            display_print(
                "Program:  NEW  LOAD\"f\"  SAVE\"f\"  RUN  LIST [n[-m]]  DELETE n[-m]  RENUM\n"
                "Files:    FILES  DIR  KILL\"f\"  RENAME\"old\",\"new\"\n"
                "Dir:      CHDIR/CD \"path\"  MKDIR \"path\"  RMDIR \"path\"\n"
                "Shell:    SYSTEM (exit)\n"
            );

        } else if (isdigit((unsigned char)*p)) {
            /* Numbered line: add/replace in program store */
            int num = (int)strtol(p, (char **)&p, 10);
            while (isspace((unsigned char)*p)) p++;
            int w = 0;
            for (int i = 0; i < g_nlines; i++)
                if (g_lines[i].linenum != num) g_lines[w++] = g_lines[i];
            g_nlines = w;
            if (*p) {
                if (g_nlines >= MAX_LINES) { display_print("Too many lines\n"); continue; }
                int ins = g_nlines;
                for (int i = 0; i < g_nlines; i++)
                    if (g_lines[i].linenum > num) { ins = i; break; }
                memmove(&g_lines[ins+1], &g_lines[ins], (g_nlines-ins)*sizeof(Line));
                g_lines[ins].linenum = num;
                strncpy(g_lines[ins].text, p, MAX_LINE_LEN-1);
                g_lines[ins].text[MAX_LINE_LEN-1] = '\0';
                g_nlines++;
            }
            continue;  /* no Ok prompt */

        } else {
            /* Immediate execution */
            Interp ip = { .pc=0, .running=1 };
            dispatch_one(&ip, p, p);
            continue;
        }
    }

    display_shutdown();
    return 0;
}

BASIC_NS_END

/* ================================================================
 * C-linkage entry points — outside the namespace.
 *
 *  WOPR build:         extern "C" int basic_main(void)
 *  FELIX_BASIC build:  extern "C" int fb_basic_main(void)
 *  Standalone:         int main(int argc, char **argv)
 * ================================================================ */
#if defined(WOPR) || defined(FELIX_BASIC)
int basic_main(void) {
    return BASIC_NS::basic_main();
}
#else
int main(int argc, char **argv) {
    return BASIC_NS::basic_main(argc, argv);
}
#endif
