/*
 * batsc - a tiny interpreter for the subset of bats used by Exercism's jq track.
 * No shell required: it parses the .bats file itself and fork/execs jq directly.
 *
 *   cc -O2 -o batsc batsc.c
 *   BATS_RUN_SKIPPED=true ./batsc test-two-fer.bats > results.json
 *
 * Supported: @test '...' { ... }, run CMD [<< 'TAG' heredoc], VAR='...' / VAR="...",
 *            skip, `[[ ... ]] || skip`, assert_success, assert_failure,
 *            assert_equal, assert_output [--partial], refute_output, assert_line [--index N].
 * Anything else inside a test is reported as an error (never silently passed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <stdarg.h>

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
static int split_words(const char *line, char **argv, char **heredoc) {
    int argc = 0; *heredoc = NULL;
    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
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
            if (*p == '\'') { p++; while (*p && *p != '\'') bput(&w, *p++); if (*p) p++; }
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

/* ---------- run: fork/exec with stdin from heredoc, capture stdout+stderr ---------- */
static void run_cmd(char **argv, const char *stdin_data) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) { perror("pipe"); exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);   /* bats `run` merges stderr into $output */
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "%s: command not found\n", argv[0]); _exit(127);
    }
    close(in[0]); close(out[1]);
    /* NB: fine for small inputs; a large heredoc + large output would need poll() to avoid deadlock */
    if (stdin_data) { size_t len = strlen(stdin_data); (void)!write(in[1], stdin_data, len); }
    close(in[1]);
    buf b = {0}; bput(&b, 0); b.n = 0;
    char tmp[4096]; ssize_t r;
    while ((r = read(out[0], tmp, sizeof tmp)) > 0) for (ssize_t i = 0; i < r; i++) bput(&b, tmp[i]);
    close(out[0]);
    int ws; waitpid(pid, &ws, 0);
    status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
    while (b.n && b.s[b.n - 1] == '\n') b.s[--b.n] = 0;        /* like $(...) */
    free(output); output = b.s;
    for (int i = 0; i < nlines; i++) free(lines_arr[i]);
    nlines = 0;
    char *copy = xstrdup(output), *save = copy, *tok;
    while ((tok = strsep(&copy, "\n")) && nlines < 1024) lines_arr[nlines++] = xstrdup(tok);
    free(save);
}

/* ---------- test bookkeeping ---------- */
typedef struct { char *name; char *code; char *message; int state; /* 0 pass 1 fail 2 error 3 skip */ } test_t;
static test_t tests[256]; static int ntests;

static void fail(test_t *t, int state, const char *fmt, ...) {
    if (t->state == 1 || t->state == 2) return;      /* keep first failure */
    char m[8192]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    t->state = state; t->message = xstrdup(m);
}

static int is_word(const char *s, const char *w) { return !strcmp(s, w); }

/* Execute one test body (array of lines). */
static void exec_test(test_t *t, char **body, int n) {
    for (int i = 0; i < n && t->state == 0; i++) {
        const char *line = body[i];
        while (isspace((unsigned char)*line)) line++;
        if (!*line || *line == '#') continue;

        /* skip guards */
        if (strstr(line, "|| skip")) {
            /* `[[ $BATS_RUN_SKIPPED == "true" ]] || skip` : skip unless the env var says run everything */
            const char *e = getenv("BATS_RUN_SKIPPED");
            if (!(e && !strcmp(e, "true"))) { t->state = 3; return; }
            continue;
        }
        if (is_word(line, "skip") || !strncmp(line, "skip ", 5)) { t->state = 3; return; }

        /* VAR=value assignment */
        const char *eq = strchr(line, '=');
        if (eq) {
            int ok = 1; for (const char *q = line; q < eq; q++) if (!(isalnum((unsigned char)*q) || *q == '_')) { ok = 0; break; }
            if (ok && eq > line) {
                char name[64]; snprintf(name, sizeof name, "%.*s", (int)(eq - line), line);
                char *av[MAXARGS], *hd; int ac = split_words(eq + 1, av, &hd);
                setvar(name, ac ? av[0] : "");
                continue;
            }
        }

        char *av[MAXARGS], *hd; int ac = split_words(line, av, &hd);
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
            }
            run_cmd(av + 1, stdin_data);
            free(stdin_data);
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
        else fail(t, 2, "unsupported statement: %s", line);
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
    static char *lines[MAXLINES]; int n = 0; char lb[8192];
    while (n < MAXLINES && fgets(lb, sizeof lb, f)) { lb[strcspn(lb, "\n")] = 0; lines[n++] = xstrdup(lb); }
    fclose(f);

    /* Parse: find `@test 'name' {` ... `}` blocks. Everything at top level (load, comments) is ignored. */
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        if (strncmp(l, "@test", 5)) continue;
        char *av[MAXARGS], *hd; split_words(l, av, &hd);
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
