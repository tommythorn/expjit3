# The 30 min optimizing native code compiler

As it happens, a question on comp.compilers got dropping keystrokes to
hack up a small tutorial-style example of how to write a compiler,
hopefully illustrating

 * lexical analysis,
 * recursive descent parsing (with minimal error handling),
 * AST generation,
 * AST transformation, and
 * (dynamic) native code generation.

In the interest of keeping this short and digestible, the language covered is
exceedingly simple: just integer expression with variables, integer constants,
additions, products, and parentheses. Still, it illustrates a number of points
and can be extended in a straight forward manner to a more realistic language.

Enough talk, grab the expjit3.c 30 min optimizing native code
compiler and compile it on an x86 box in 32-bit mode.

Obviously, this example doesn't really cover things such as symbol
table, control issues ("statements"), variable management,
subroutines/functions, register allocation, etc.  In a few days I'll
add a more complex example covering some of these issues.

Last update: 2006-09-21

