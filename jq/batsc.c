/*
 * batsc - a tiny interpreter for the subset of bats used by Exercism's jq track.
 * No shell required: it parses the .bats file itself and fork/execs jq directly.
 *
 *   cc -O2 -o batsc batsc.c
 *   BATS_RUN_SKIPPED=true ./batsc test-two-fer.bats > results.json
 *
 * Supported: @test '...' { ... }, run CMD [<< TAG heredoc | <<< 'string'] (args may span
 *            lines inside quotes or with a trailing backslash), VAR='...' / VAR="...", ${#lines[@]},
 *            VAR=$(CMD << TAG ... TAG ), `< file` stdin redirection, skip, `[[ ... ]] || skip`, assert_success, assert_failure,
 *            assert_equal, assert_output [--partial], refute_output, assert_line [--index N],
 *            and from bats-jq.bash: assert_objects_equal, assert_float [-d N], assert_key_value.
 *            stderr lines starting with `["DEBUG:",` are dropped from $output, as bats-jq.bash does.
 * Anything else inside a test is reported as an error (never silently passed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

#define MAXLINES 4096
#define MAXVARS  64
#define MAXARGS  64

/* ---------- variables ---------- */
static struct { char *k, *v; } vars[MAXVARS];
static int nvars;
static char *output;            /* $output from last `run`  */
static int status = -1;         /* $status                  */
static char *lines_arr[1024];   /* ${lines[N]}              */
static int nlines;

static char *xstrdup(const char *s) { char *d = strdup(s); if (!d) { perror("strdup"); exit(1); } return d; }

static const char *getvar(const char *k) {
    if (!strcmp(k, "output")) return output ? output : "";
    if (!strcmp(k, "status")) { static char b[16]; snprintf(b, sizeof b, "%d", status); return b; }
    if (!strcmp(k, "#lines[@]")) { static char b[16]; snprintf(b, sizeof b, "%d", nlines); return b; }
    if (!strncmp(k, "lines[", 6)) { int i = atoi(k + 6); return (i >= 0 && i < nlines) ? lines_arr[i] : ""; }
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].k, k)) return vars[i].v;
    const char *e = getenv(k);
    return e ? e : "";
}
static void setvar(const char *k, const char *v) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].k, k)) { free(vars[i].v); vars[i].v = xstrdup(v); return; }
    if (nvars < MAXVARS) { vars[nvars].k = xstrdup(k); vars[nvars].v = xstrdup(v); nvars++; }
}

/* ---------- growable string ---------- */
typedef struct { char *s; size_t n, cap; } buf;
static void bput(buf *b, char c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 128; b->s = realloc(b->s, b->cap); }
    b->s[b->n++] = c; b->s[b->n] = 0;
}
static void bputs(buf *b, const char *s) { while (*s) bput(b, *s++); }

/* ---------- shell-ish word splitting ---------- */
/* Splits `line` into argv honouring '...', "..." (with $var / ${var} expansion) and \ escapes.
 * Stops at `<<` and returns the heredoc tag via *heredoc (or NULL). */
static char *stdin_file;   /* set by split_words when it sees `< file` */
static int split_words(const char *line, char **argv, char **heredoc, char **herestring) {
    int argc = 0; *heredoc = NULL; if (herestring) *herestring = NULL; stdin_file = NULL;
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (p[0] == '<' && p[1] == '<' && p[2] == '<') {   /* here-string: <<< 'text' */
            char *sub[2], *dummy; p += 3;
            if (split_words(p, sub, &dummy, NULL) && herestring) *herestring = sub[0];
            break;
        }
        if (p[0] == '<' && p[1] != '<') {           /* stdin redirect: < file */
            char *sub[2], *dummy; p++;
            if (split_words(p, sub, &dummy, NULL)) stdin_file = sub[0];
            break;
        }
        if (p[0] == '<' && p[1] == '<') {           /* heredoc: << 'TAG' | << "TAG" | << TAG | <<- */
            p += 2; if (*p == '-') p++;
            while (isspace((unsigned char)*p)) p++;
            buf t = {0};
            char q = (*p == '\'' || *p == '"') ? *p++ : 0;
            while (*p && (q ? *p != q : !isspace((unsigned char)*p))) bput(&t, *p++);
            *heredoc = t.s ? t.s : xstrdup("");
            break;
        }
        buf w = {0}; bput(&w, 0); w.n = 0;           /* make sure w.s is non-NULL even for "" */
        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '$' && p[1] == '\'') {           /* $'...' ANSI-C quoting */
                p += 2;
                while (*p && *p != '\'') {
                    if (*p == '\\' && p[1]) {
                        p++;
                        switch (*p) {
                        case 'n': bput(&w, '\n'); break; case 't': bput(&w, '\t'); break;
                        case 'r': bput(&w, '\r'); break; case 'e': bput(&w, 27); break;
                        case 'a': bput(&w, 7); break;    case '0': bput(&w, 0); break;
                        default: bput(&w, *p);
                        }
                        p++;
                    } else bput(&w, *p++);
                }
                if (*p) p++;
            }
            else if (*p == '\'') { p++; while (*p && *p != '\'') bput(&w, *p++); if (*p) p++; }
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) { p++; bput(&w, *p++); }
                    else if (*p == '$') {
                        p++; char name[64]; int n = 0;
                        if (*p == '{') { p++; while (*p && *p != '}' && n < 63) name[n++] = *p++; if (*p) p++; }
                        else while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
                        name[n] = 0; bputs(&w, getvar(name));
                    } else bput(&w, *p++);
                }
                if (*p) p++;
            }
            else if (*p == '\\' && p[1]) { p++; bput(&w, *p++); }
            else if (*p == '$') {
                p++; char name[64]; int n = 0;
                if (*p == '{') { p++; while (*p && *p != '}' && n < 63) name[n++] = *p++; if (*p) p++; }
                else while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
                name[n] = 0; bputs(&w, getvar(name));
            }
            else if (*p == '#' && w.n == 0) { while (*p) p++; break; }  /* trailing comment */
            else bput(&w, *p++);
        }
        if (argc < MAXARGS - 1) argv[argc++] = w.s;
    }
    argv[argc] = NULL;
    return argc;
}

/* ---------- run: fork/exec with stdin from data, capture stdout and stderr ---------- */
/* Runs argv, feeding stdin_data (may be NULL). Returns the exit status; *out gets stdout and
 * *err gets stderr (both malloc'd, trailing newlines intact). */
static int capture(char **argv, const char *stdin_data, char **out, char **err) {
    int in[2], outp[2];
    char errpath[] = "/tmp/batsc-err-XXXXXX";
    int errfd = mkstemp(errpath);
    if (errfd < 0 || pipe(in) || pipe(outp)) { perror("pipe"); exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        dup2(in[0], 0); dup2(outp[1], 1); dup2(errfd, 2);
        if (stdin_file) { FILE *sf = fopen(stdin_file, "r"); if (sf) dup2(fileno(sf), 0); else { perror(stdin_file); _exit(1); } }
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]); close(errfd);
        execvp(argv[0], argv);
        fprintf(stderr, "%s: command not found\n", argv[0]); _exit(127);
    }
    close(in[0]); close(outp[1]);
    /* NB: fine for small inputs; a large heredoc + large output would need poll() to avoid deadlock */
    if (stdin_data) { size_t len = strlen(stdin_data); (void)!write(in[1], stdin_data, len); }
    close(in[1]);
    buf b = {0}; bput(&b, 0); b.n = 0;
    char tmp[4096]; ssize_t r;
    while ((r = read(outp[0], tmp, sizeof tmp)) > 0) for (ssize_t i = 0; i < r; i++) bput(&b, tmp[i]);
    close(outp[0]);
    int ws; waitpid(pid, &ws, 0);
    buf e = {0}; bput(&e, 0); e.n = 0;
    lseek(errfd, 0, SEEK_SET);
    while ((r = read(errfd, tmp, sizeof tmp)) > 0) for (ssize_t i = 0; i < r; i++) bput(&e, tmp[i]);
    close(errfd); unlink(errpath);
    *out = b.s; *err = e.s;
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
}

/* Strip trailing newlines in place, like $(...) does. */
static void chomp(char *s) { size_t n = strlen(s); while (n && s[n - 1] == '\n') s[--n] = 0; }

/* bats `run`: sets $output, $status and ${lines[]}.
 * Mirrors the jq() wrapper in bats-jq.bash: stderr lines starting with `["DEBUG:",` are
 * diagnostic and dropped; remaining stderr lines come first, then stdout. */
static void run_cmd(char **argv, const char *stdin_data) {
    char *out, *err;
    status = capture(argv, stdin_data, &out, &err);
    buf b = {0}; bput(&b, 0); b.n = 0;
    char *copy = xstrdup(err), *save = copy, *tok;
    chomp(copy);
    if (*copy) while ((tok = strsep(&copy, "\n")))
        if (strncmp(tok, "[\"DEBUG:\",", 10)) { bputs(&b, tok); bput(&b, '\n'); }
    free(save);
    bputs(&b, out); free(out); free(err);
    chomp(b.s); b.n = strlen(b.s);
    free(output); output = b.s;
    for (int i = 0; i < nlines; i++) free(lines_arr[i]);
    nlines = 0;
    copy = xstrdup(output); save = copy;
    while ((tok = strsep(&copy, "\n")) && nlines < 1024) lines_arr[nlines++] = xstrdup(tok);
    free(save);
}

/* Run jq with the given args (NULL-terminated) and stdin; return chomped stdout. */
static char *jq_capture(const char *stdin_data, ...) {
    char *av[MAXARGS]; int ac = 0; av[ac++] = "jq";
    va_list ap; va_start(ap, stdin_data);
    char *a; while ((a = va_arg(ap, char *)) && ac < MAXARGS - 1) av[ac++] = a;
    va_end(ap); av[ac] = NULL;
    char *out, *err; capture(av, stdin_data, &out, &err); free(err);
    chomp(out); return out;
}

/* ---------- test bookkeeping ---------- */
typedef struct { char *name; char *code; char *message; int state; /* 0 pass 1 fail 2 error 3 skip */ } test_t;
static test_t tests[256]; static int ntests;

static void fail(test_t *t, int state, const char *fmt, ...) {
    if (t->state == 1 || t->state == 2) return;      /* keep first failure */
    char *m = NULL; va_list ap; va_start(ap, fmt); if (vasprintf(&m, fmt, ap) < 0) m = xstrdup("?"); va_end(ap);
    t->state = state; t->message = m;
}

static int is_word(const char *s, const char *w) { return !strcmp(s, w); }

/* Does `s` end inside an open quote, or with a line continuation? */
static int statement_continues(const char *s) {
    char q = 0; size_t len = strlen(s);
    for (const char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; else if (q == '"' && *p == '\\' && p[1]) p++; }
        else if (*p == '\'' || *p == '"') q = *p;
        else if (*p == '\\' && p[1]) p++;
        else if (*p == '#') break;
    }
    return q || (len && s[len - 1] == '\\' && !q);
}

/* Join body[*i..] into one logical statement while quotes are open or a line ends in `\`. */
static char *join_statement(char **body, int *i, int n) {
    buf b = {0}; bput(&b, 0); b.n = 0;
    bputs(&b, body[*i]);
    while (statement_continues(b.s) && *i + 1 < n) {
        if (b.n && b.s[b.n - 1] == '\\') b.s[--b.n] = 0;   /* drop the continuation backslash */
        else bput(&b, '\n');                                /* keep newlines inside quotes */
        bputs(&b, body[++*i]);
    }
    return b.s;
}

/* Execute one test body (array of lines). */
static void exec_test(test_t *t, char **body, int n) {
    for (int i = 0; i < n && t->state == 0; i++) {
        char *joined = join_statement(body, &i, n);
        const char *line = joined;
        while (isspace((unsigned char)*line)) line++;
        if (!*line || *line == '#') { free(joined); continue; }

        /* skip guards */
        if (strstr(line, "|| skip")) {
            /* `[[ $BATS_RUN_SKIPPED == "true" ]] || skip` : skip unless the env var says run everything */
            const char *e = getenv("BATS_RUN_SKIPPED");
            if (!(e && !strcmp(e, "true"))) { t->state = 3; return; }
            free(joined); continue;
        }
        if (is_word(line, "skip") || !strncmp(line, "skip ", 5)) { t->state = 3; return; }

        /* VAR=value assignment */
        const char *eq = strchr(line, '=');
        if (eq) {
            int ok = 1; for (const char *q = line; q < eq; q++) if (!(isalnum((unsigned char)*q) || *q == '_')) { ok = 0; break; }
            if (ok && eq > line) {
                char name[64]; snprintf(name, sizeof name, "%.*s", (int)(eq - line), line);
                const char *val = eq + 1;
                if (!strncmp(val, "$(", 2) && strstr(val, "<<")) {
                    /* expected=$(CMD ... << TAG ... TAG  followed by a line with `)` */
                    char *av[MAXARGS], *hd; int ac = split_words(val + 2, av, &hd, NULL);
                    buf b = {0}; bput(&b, 0); b.n = 0;
                    for (i++; i < n; i++) {
                        if (!strcmp(body[i], hd ? hd : "")) break;
                        bputs(&b, body[i]); bput(&b, '\n');
                    }
                    for (i++; i < n; i++) {          /* skip to the closing `)` */
                        const char *l = body[i]; while (isspace((unsigned char)*l)) l++;
                        if (*l == ')') break;
                    }
                    if (ac == 1 && is_word(av[0], "cat")) { chomp(b.s); setvar(name, b.s); }
                    else {
                        char *out, *err; capture(av, b.s, &out, &err);
                        chomp(out); setvar(name, out); free(out); free(err);
                    }
                    free(b.s); free(joined); continue;
                }
                char *av[MAXARGS], *hd; int ac = split_words(val, av, &hd, NULL);
                setvar(name, ac ? av[0] : "");
                free(joined); continue;
            }
        }

        char *av[MAXARGS], *hd, *hs; int ac = split_words(line, av, &hd, &hs);
        if (ac == 0) continue;

        if (is_word(av[0], "run")) {
            if (ac < 2) { fail(t, 2, "run: no command"); return; }
            char *stdin_data = NULL;
            if (hd) {                                  /* collect heredoc body until TAG */
                buf b = {0}; bput(&b, 0); b.n = 0;
                for (i++; i < n; i++) {
                    const char *l = body[i]; while (isspace((unsigned char)*l)) l++;
                    if (!strcmp(l, hd)) break;
                    bputs(&b, body[i]); bput(&b, '\n');
                }
                stdin_data = b.s;
            } else if (hs) {
                buf b = {0}; bput(&b, 0); b.n = 0; bputs(&b, hs); bput(&b, '\n');
                stdin_data = b.s;
            }
            run_cmd(av + 1, stdin_data);
            free(stdin_data); stdin_file = NULL;
        }
        else if (is_word(av[0], "assert_success")) {
            if (status != 0) fail(t, 1, "command failed with status %d\noutput: %s", status, output);
        }
        else if (is_word(av[0], "assert_failure")) {
            if (status == 0) fail(t, 1, "command succeeded, but failure was expected\noutput: %s", output);
            else if (ac > 1 && atoi(av[1]) != status) fail(t, 1, "expected status %s, got %d", av[1], status);
        }
        else if (is_word(av[0], "assert_equal")) {
            if (ac < 3) fail(t, 2, "assert_equal needs two args");
            else if (strcmp(av[1], av[2])) fail(t, 1, "values do not equal\nexpected : %s\nactual   : %s", av[2], av[1]);
        }
        else if (is_word(av[0], "assert_output") || is_word(av[0], "refute_output")) {
            int partial = 0, k = 1;
            if (k < ac && (!strcmp(av[k], "--partial") || !strcmp(av[k], "-p"))) { partial = 1; k++; }
            const char *exp = k < ac ? av[k] : "";
            int hit = partial ? strstr(output, exp) != NULL : !strcmp(output, exp);
            int refute = av[0][0] == 'r';
            if (hit == refute) fail(t, 1, "%s\nexpected : %s\nactual   : %s",
                                   refute ? "output should not match" : "output differs", exp, output);
        }
        else if (is_word(av[0], "assert_line")) {
            int idx = -1, k = 1;
            if (k + 1 < ac && !strcmp(av[k], "--index")) { idx = atoi(av[k + 1]); k += 2; }
            const char *exp = k < ac ? av[k] : "";
            int hit = 0;
            if (idx >= 0) hit = idx < nlines && !strcmp(lines_arr[idx], exp);
            else for (int j = 0; j < nlines; j++) if (!strcmp(lines_arr[j], exp)) hit = 1;
            if (!hit) fail(t, 1, "line not found\nexpected : %s\noutput   : %s", exp, output);
        }
        /* --- assertions from bats-jq.bash --- */
        else if (is_word(av[0], "assert_objects_equal")) {
            if (ac < 3) { fail(t, 2, "assert_objects_equal needs two args"); }
            else {
                char *r = jq_capture(NULL, "-n", "--argjson", "actual", av[1], "--argjson", "expected", av[2],
                                     "$actual == $expected", (char *)NULL);
                if (strcmp(r, "true")) fail(t, 1, "objects do not equal\nexpected : %s\nactual   : %s", av[2], av[1]);
                free(r);
            }
        }
        else if (is_word(av[0], "assert_float")) {
            int decimals = 2, k = 1;
            if (k + 1 < ac && !strcmp(av[k], "-d")) { decimals = atoi(av[k + 1]); k += 2; }
            else if (k < ac && !strncmp(av[k], "-d", 2)) { decimals = atoi(av[k] + 2); k++; }
            if (k < ac && !strcmp(av[k], "--")) k++;
            if (k + 1 >= ac) fail(t, 2, "assert_float needs two values");
            else {
                double m = pow(10, decimals);
                double a = trunc(strtod(av[k], NULL) * m) / m, e = trunc(strtod(av[k + 1], NULL) * m) / m;
                if (a != e) fail(t, 1, "values do not equal\nexpected : %.*f\nactual   : %.*f", decimals, e, decimals, a);
            }
        }
        else if (is_word(av[0], "assert_key_value")) {
            if (ac < 3) fail(t, 2, "assert_key_value needs key and value");
            else {
                buf in = {0}; bput(&in, 0); in.n = 0; bputs(&in, output ? output : ""); bput(&in, '\n');
                char *r = jq_capture(in.s, "-rc", "--arg", "key", av[1], ".[$key]", (char *)NULL);
                if (strcmp(r, av[2])) fail(t, 1, "values do not equal\nexpected : %s\nactual   : %s", av[2], r);
                free(r); free(in.s);
            }
        }
        else fail(t, 2, "unsupported statement: %s", line);
        free(joined);
    }
}

/* ---------- JSON output ---------- */
static void jstr(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        switch (*s) {
        case '"': fputs("\\\"", stdout); break; case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;  case '\t': fputs("\\t", stdout); break;
        default: if ((unsigned char)*s < 0x20) printf("\\u%04x", *s); else putchar(*s);
        }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE.bats\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r"); if (!f) { perror(argv[1]); return 2; }
    static char *lines[MAXLINES]; int n = 0; char *lb = NULL; size_t cap = 0;
    while (n < MAXLINES && getline(&lb, &cap, f) != -1) { lb[strcspn(lb, "\n")] = 0; lines[n++] = xstrdup(lb); }
    free(lb);
    fclose(f);

    /* Parse: find `@test 'name' {` ... `}` blocks. Everything at top level (load, comments) is ignored. */
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        if (strncmp(l, "@test", 5)) continue;
        char *av[MAXARGS], *hd; split_words(l, av, &hd, NULL);
        test_t *t = &tests[ntests++];
        t->name = xstrdup(av[1] ? av[1] : "?"); t->state = 0; t->message = NULL;
        int start = ++i; buf code = {0}; bput(&code, 0); code.n = 0;
        while (i < n && strcmp(lines[i], "}")) { bputs(&code, lines[i]); bput(&code, '\n'); i++; }
        t->code = code.s;
        exec_test(t, lines + start, i - start);
    }

    int any_fail = 0;
    for (int i = 0; i < ntests; i++) if (tests[i].state == 1 || tests[i].state == 2) any_fail = 1;
    static const char *names[] = { "pass", "fail", "error", "skip" };
    printf("{\"version\":2,\"status\":\"%s\",\"tests\":[", any_fail ? "fail" : "pass");
    for (int i = 0; i < ntests; i++) {
        test_t *t = &tests[i];
        printf("%s{\"name\":", i ? "," : ""); jstr(t->name);
        printf(",\"status\":\"%s\",\"test_code\":", names[t->state]); jstr(t->code);
        if (t->message) { printf(",\"message\":"); jstr(t->message); }
        putchar('}');
    }
    puts("]}");
    return any_fail;
}
