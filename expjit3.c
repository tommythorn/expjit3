/*
 * Tutorial example compiler for a tiny expression language, hopefully
 * illustrating
 * - lexical analysis,
 * - recursive descent parsing (with minimal error handling),
 * - AST generation,
 * - AST transformation, and
 * - (dynamic) native code generation.
 *
 * This example expects to be run on a 32-bit x86.  For 64-bit Linux,
 * compile with gcc -m32.
 *
 * Tommy Thorn 2006-09-18, placed in the public domain.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/*
 * Lexical analysis
 */

typedef enum {
        END_OF_FILE = 0,
        ERROR = 1,
        // One-char tokens represent themselves
        INT = 256,
        NAME,
} token_t;

static char    *s;		// Source code pointer
static token_t  lookahead;
static int      intValue;
static char    *symbolValue;	// Pointing to a place in the source.
static unsigned symbolLength;  // Length of same (which is *not* zero terminated)
                        // XXX Not used in this example

/*
 * Produce the next token in `lookahead' from the source code pointed
 * by `s'.  Integer constants and name leave information in auxiliary
 * variables intValue, symbolValue, and symbolValue.
 */
static void nexttoken(void)
{
        while (isspace(*s))
                ++s;

        if (isdigit(*s)) {
                intValue = 0;
                lookahead = INT;
                while (isdigit(*s))
                        intValue = 10*intValue + *s++ - '0';
        } else if (isalpha(*s)) {
                lookahead = NAME;
                symbolValue = s;
                while (isalnum(*s))
                        ++s;
                symbolLength = symbolValue - s;
        } else
                lookahead = *s++;
}

static void match(token_t expect)
{
        if (lookahead != expect)
                lookahead = ERROR; // Terminate all parsing
        else
                nexttoken();
}


/*
 * Abstract Syntax Tree (AST).
 *
 * In general this will be a structure with a tag and a union of
 * alternatives corresponding to the tag, but for this simple example,
 * we can get away with some abuse of structure fields.
 */

typedef struct node *ast_t;
static struct node {
        token_t kind;	// great correspondence means we reuse the type here
        ast_t l, r;	// left and right subtrees
        int intValue;	// irrelevant unless kind == INT
        int shared;	// this node is shared (eg. a common sub expression)
        int *saved;	// for the code generation
} nodes[9999];

static int next = 0;

/*
 * Building the AST is a key operation as this is a prime opportunity
 * to transform the internal representation.  Notice how it calls upon
 * itself recursively to simplify subtrees.
 *
 * Some of the transformation here (like the x+x -> 2*x) are
 * undesirable on their own, but are included to expose more
 * opportunities for other transformations.
 *
 * XXX There is a small bug below: the rewriting can cause nodes to
 * become unrefererened ("garbage"), which is fine. However, if such a
 * node is subsequently rediscovered by the CSE it may be marked as
 * shared when it really isn't and thus cause a needless saving of the
 * value in codegen. There are several ways to fix it, but as none of
 * them are very elegent and the bug fairly innocent, I'll leave it
 * unfixed for now. (FWIW, the best way to fix this is to delay the
 * marking of nodes as "shared" until after parsing, but before code
 * generation, using a new tree traversal).
 */
static ast_t mk(token_t kind, ast_t l, ast_t r, int k)
{
        // CSE
        ast_t p, t;
        if ((kind == '*' || kind == '+') && l->kind == INT)
                t = l, l = r, r = t; // move constants to the right

        for (p = &nodes[0]; p != &nodes[next]; ++p)
                if (p->kind == kind && p->l == l && p->r == r && p->intValue == k) {
                        p->shared = 1;
                        return p;
                }

        // Constant folding (partially)
        // k1 + k2 -> [k1 + k2]
        if (kind == '+' && l->kind == INT && r->kind == INT)
                return mk(INT, 0, 0, l->intValue + r->intValue);
        // k1 * k2 -> [k1 * k2]
        if (kind == '*' && l->kind == INT && r->kind == INT)
                return mk(INT, 0, 0, l->intValue * r->intValue);

        // Dead code elimination / alg. simplification
        // x * 0 -> 0
        if (kind == '*' && r->kind == INT && r->intValue == 0)
                return mk(INT, 0, 0, 0);
        // x * 1 -> x
        if (kind == '*' && r->kind == INT && r->intValue == 1)
                return l;
        // x + 0 -> x
        if (kind == '+' && r->kind == INT && r->intValue == 0)
                return l;

        // x + x -> 2 * x
        if (kind == '+' && r == l)
                return mk('*', mk(INT,0,0,2), l, 0);

        // Move constants to the right
        // (x + k1) + y -> x + (y + k1)
        if (kind == '+' && l->kind == '+' && l->r->kind == INT)
                return mk('+', l->l, mk('+', r, l->r, 0), 0);
        if (kind == '*' && l->kind == '*' && l->r->kind == INT)
                return mk('*', l->l, mk('*', r, l->r, 0), 0);

        // (x + k2) * k1 -> x * k1 + k2 * k1
        if (kind == '*' && r->kind == INT && l->kind == '+' && l->r->kind == INT)
                return mk('+',
                          mk('*',l->l,r,0),
                          mk('*',l->r,r,0),
                          0);

        nodes[next].kind = kind;
        nodes[next].l = l;
        nodes[next].r = r;
        nodes[next].intValue = k;
        nodes[next].shared = 0;
        nodes[next].saved = 0;
        return &nodes[next++];
}


/*
 * Recursive descent parsing.
 *
 * This is completely standard, see any good compiler book (like the
 * Dragon book).
 */

static ast_t pExp(void);
static ast_t pFactor(void)
{
        ast_t v;
        switch (lookahead) {
        case '(':
                match('('); v = pExp(); match(')');
                break;

        case NAME:
                v = mk(NAME, 0,0, symbolValue[0]); match(NAME);
                break;

        case INT:
                v = mk(INT, 0,0, intValue); match(INT);
                break;
        }

        return v;
}

static ast_t pTerm(void)
{
        ast_t v = pFactor();
        while (lookahead == '*')
                match('*'), v = mk('*',v,pFactor(),0);
        return v;
}

static ast_t pExp(void)
{
        ast_t v = pTerm();
        while (lookahead == '+')
                match('+'), v = mk('+',v,pTerm(),0);

        return v;
}


/*
 * Unparsing the AST.
 *
 * Both serving as an example of AST traversal and a debugging tool
 * for examining the result of transformations.
 */
static void unparse(ast_t t)
{
        if (t->kind == INT)
                printf("%d", t->intValue);
        else if (t->kind == NAME)
                printf("%c", t->intValue);
        else {
                printf("(");
                if (t->shared)
                        putchar('!');
                unparse(t->l);
                printf("%c", t->kind);
                unparse(t->r);
                printf(")");
        }
}


/*
 * Code generation.
 *
 * Classic template expansion. For a stack machine code generation is
 * trivial. Expressions are compiled to leave their results in the
 * accumulator.  Binary expressions save intermediate values on the
 * stack until they are ready to produce their result.
 *
 * Common subexpressions are here treated as implicitly store to a
 * local variable.  In a real compiler, this would be more explicit at
 * a higher level, and no special treatment needed here.
 *
 * Ironically, it's actually much simpler to directly generate native
 * x86 code here than some assembler output.
 *
 * Symbol table handling is unrealistically simplistic here.
 */

static char *code, *cp;
static int env[256] = { ['x'] = 2, ['y'] = 3 };
static int instructions = 0;
static int cse_values[9999], *cse_p = cse_values;

static void codegen(ast_t t)
{
        if (t->kind == INT) {
                *cp++ = 0xB8; // movl $<intValue>, %eax
                *(int *)cp = t->intValue;
                cp += 4;
                ++instructions;
        } else if (t->kind == NAME) {
                *cp++ = 0xA1; // mov <env[..]>, %eax
                *(int **)cp = &env[t->intValue];
                cp += 4;
                ++instructions;
        } else {
                // CSE part 2
                if (t->saved) {
                        *cp++ = 0xA1; // mov <env[..]>, %eax
                        *(int **)cp = t->saved;
                        cp += 4;
                        ++instructions;
                        return;
                }

                codegen(t->r);
                *cp++ = 0x50; // push %eax
                codegen(t->l);
                *cp++ = 0x5B; // pop %ebx
                if (t->kind == '+')
                        *cp++ = 1, *cp++ = 0xD8; // add %ebx,%eax
                else
                        *cp++ = 0xF, *cp++ = 0xAF, *cp++ = 0xC3; // imul %ebx, %eax
                instructions += 2;

                // CSE part 3
                if (t->shared) {
                        *cp++ = 0xA3; // mov %eax, <...>
                        *(int **)cp = t->saved = cse_p++;
                        cp += 4;
                        ++instructions;
                        return;
                }
        }
}


/*
 * Main.
 */

// Allocates RWX memory of given size and returns a pointer to it. On failure,
// prints out the error and returns NULL.
void* alloc_executable_memory(size_t size)
{
        void* ptr = mmap(0, size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == (void*)-1) {
                perror("mmap");
                return NULL;
        }
        return ptr;
}




typedef int (*int_function_pointer)();

int main(int argc, char **argv)
{
        ast_t res;
        s = "(1 + x*3 + 4*(5 + y)) * (1 + x*3 + 4*(5 + y))";
        if (argc > 1)
                s = argv[1];
        nexttoken();
        res = pExp();
        if (lookahead) {
                printf("Syntax error at:%s\n", s);
                return -1;
        }
        unparse(res);
        printf("\n");

        cp = code = alloc_executable_memory(9999);

        *cp++ = 0x53; // push %ebx
        codegen(res);
        *cp++ = 0x5B; // pop %ebx
        *cp++ = 0xC3; // ret
        ++instructions;
        printf("%d instruction, value %d\n",
               instructions,
               ((int_function_pointer) code)());  // cast the code pointer and call it.

        return 0;
}
