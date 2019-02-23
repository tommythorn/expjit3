/*
 * Tutorial example compiler for a tiny expression language, hopefully
 * illustrating
 * - lexical analysis,
 * - recursive descent parsing (with minimal error handling),
 * - AST generation,
 * - AST transformation, and
 * - (dynamic) native code generation.
 *
 * This example expects to be run on a RISC-V RV64GC.
 *
 * Tommy Thorn 2006-09-18, placed in the public domain.
 * Tommy Thorn 2019-02-22, ported to RISC-V
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

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

static char    *s;              // Source code pointer
static token_t  lookahead;
static int      intValue;
static char    *symbolValue;    // Pointing to a place in the source.
static unsigned symbolLength;   // Length of same (which is *not* zero terminated)
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
        token_t kind;   // great correspondence means we reuse the type here
        ast_t l, r;     // left and right subtrees
        int intValue;   // irrelevant unless kind == INT

        int shared;     // this node is shared (eg. a common sub expression)
        int alloc;      // if nonzero, the desired register
        int reg;        // for the code generation
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
                        p->shared++;
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
        nodes[next].shared = 1;
        nodes[next].alloc = 0;
        nodes[next].reg = 0;
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
 * Classic template expansion.  Rather than generating fully general
 * code that stores to a stack, we cheat and pretend we have unlimited
 * number of register.  A realistic codegen would do better.
 *
 * Expressions are compiled to leave their results in registers
 * corresponding to the depth of the stack.
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

static uint32_t *code, *cp;
static int env[256] = { ['x'] = 2, ['y'] = 3 };
static int cse_values[9999], *cse_p = cse_values;

static const int reg_a0 = 10;
// free registers {t0 .. s1, a1, ... t6}
static int reg_poll[] = { 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                          21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
static int next_free = 0;

static void alloc(ast_t t)
{
        if (t->alloc) {
                t->reg = t->alloc;
                return;
        }

        assert(next_free < sizeof reg_poll / sizeof *reg_poll);
        t->reg = reg_poll[next_free++];
}

static int use(ast_t t)
{
        int r = t->reg;

        assert(t->shared > 0);
        t->shared--;
        if (t->shared == 0) {
                assert(0 < next_free);
                reg_poll[--next_free] = r;
        }

        return r;
}

static void codegen(ast_t t)
{
        int hi = 0;

        if (t->reg)
                return;

        switch (t->kind) {
        case INT:
                alloc(t);

                hi = 0;
                if (t->intValue & 0xFFFFF000) {
                        // lui $reg, %hi(t->intValue)
                        *cp++ = t->intValue & 0xFFFFF000 | t->reg << 7 | 0x37;
                        hi = t->reg;
                }

                // addi $reg, $hi, %lo(t->intValue)
                *cp++ = (t->intValue & 0xFFF) << 20 | hi << 15 | t->reg << 7 | 0x13;
                break;

        case NAME:
                alloc(t);

                // We require a0 to hold a pointer to env
                // lw $reg, off(t0)
                *cp++ = (t->intValue * 4) << 20 | 2 << 12 | reg_a0 << 15 | t->reg << 7 | 0x03;
                break;

        case '+': {
                codegen(t->l);
                codegen(t->r);

                int r = use(t->r);
                int l = use(t->l);
                alloc(t);

                // add $l, $l, $r
                *cp++ = r << 20 | l << 15 | t->reg << 7 | 0x33;
                break;
        }

        case '*': {
                codegen(t->l);
                codegen(t->r);

                int r = use(t->r);
                int l = use(t->l);
                alloc(t);

                // mul $reg, $reg, $(reg+1)
                *cp++ =  1 << 25 | r << 20 | l << 15 | t->reg << 7 | 0x33;
                break;
        }

        default:
                assert(0);
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




typedef int (*int_function_pointer)(int *);

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
        res->alloc = reg_a0;
        codegen(res);
        *cp++ = 0x8082; // c.ret

        asm("fence.i");

        printf("%d instruction, value %d\n",
               cp - code,
               ((int_function_pointer) code)(env));  // cast the code pointer and call it.

        return 0;
}
