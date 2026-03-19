lexer grammar SYsULexer;

// Int : 'int';
// Return : 'return';
Char: 'char';
Short: 'short';
Int: 'int';
Long: 'long';
Float: 'float';
Double: 'double';
Void: 'void';
Signed: 'signed';
Unsigned: 'unsigned';
Struct: 'struct';
Union: 'union';
Enum: 'enum';
Typedef: 'typedef';
Extern: 'extern';
Static: 'static';
Auto: 'auto';
Register: 'register';
Const: 'const';
Volatile: 'volatile';
Restrict: 'restrict';
Inline: 'inline';
If: 'if';
Else: 'else';
While: 'while';
Do: 'do';
For: 'for';
Switch: 'switch';
Case: 'case';
Default: 'default';
Break: 'break';
Continue: 'continue';
Return: 'return';
Goto: 'goto';
Sizeof: 'sizeof';

// LeftParen : '(';
// RightParen : ')';
// LeftBracket : '[';
// RightBracket : ']';
// LeftBrace : '{';
// RightBrace : '}';

// Plus : '+';

// Semi : ';';
// Comma : ',';

// Equal : '=';
LeftParen: '(';
RightParen: ')';
LeftBracket: '[';
RightBracket: ']';
LeftBrace: '{';
RightBrace: '}';
Plus: '+';
Minus: '-';
Star: '*';
Slash: '/';
Percent: '%';
Amp: '&';
Pipe: '|';
Caret: '^';
Tilde: '~';
Exclaim: '!';
Less: '<';
Greater: '>';
Equal: '=';
Question: '?';
Colon: ':';
Semi: ';';
Comma: ',';
Period: '.';
Arrow: '->';
PlusPlus: '++';
MinusMinus: '--';
LessLess: '<<';
GreaterGreater: '>>';
LessLessEqual: '<<=';
GreaterGreaterEqual: '>>=';
AmpAmp: '&&';
PipePipe: '||';
EqualEqual: '==';
ExclaimEqual: '!=';
LessEqual: '<=';
GreaterEqual: '>=';
PlusEqual: '+=';
MinusEqual: '-=';
StarEqual: '*=';
SlashEqual: '/=';
PercentEqual: '%=';
AmpEqual: '&=';
PipeEqual: '|=';
CaretEqual: '^=';
Ellipsis: '...';


Identifier
    :   IdentifierNondigit
        (   IdentifierNondigit
        |   Digit
        )*
    ;

fragment
IdentifierNondigit
    :   Nondigit
    ;

fragment
Nondigit
    :   [a-zA-Z_]
    ;

fragment
Digit
    :   [0-9]
    ;

Constant
    :   IntegerConstant
    |   FloatConstant
    ;

fragment
IntegerConstant
    :   DecimalConstant
    |   OctalConstant
    |   HexadecimalConstant
    ;

fragment
DecimalConstant
    :   NonzeroDigit Digit* ([uU]? [lL]? [lL]? | [lL]? [lL]? [uU]?)
    ;

fragment
OctalConstant
    :   '0' OctalDigit* ([uU]? [lL]? [lL]? | [lL]? [lL]? [uU]?)
    ;

fragment
HexadecimalConstant
    : '0'[xX] [0-9a-fA-F]+ ([uU]? [lL]? [lL]? | [lL]? [lL]? [uU]?)
    ;

fragment
NonzeroDigit
    :   [1-9]
    ;

fragment
OctalDigit
    :   [0-7]
    ;

fragment
FloatConstant
    : DecimalFloatConstant
    | HexadecimalConstant
    ;

fragment
DecimalFloatConstant
    : [0-9]* '.' [0-9]+ ([eE][+-]?[0-9]+)?
    | [0-9]+ '.' [0-9]* ([eE][+-]?[0-9]+)?
    | [0-9]+ [eE][+-]?[0-9]+
    ;

fragment
HexadecimalFloatConstant
    : '0' [xX][0-9a-fA-F]* '.' [0-9a-fA-F]+ [pP][+-]?[0-9]+ [fFlL]?
    | '0' [xX][0-9a-fA-F]+ '.' [0-9a-fA-F]* [pP][+-]?[0-9]+ [fFlL]?
    | '0' [xX][0-9a-fA-F]+ [pP][+-]?[0-9]+ [fFlL]?
    ;


// 预处理信息处理，可以从预处理信息中获得文件名以及行号
// 预处理信息中的第一个数字即为行号
LineAfterPreprocessing
    :   '#' Whitespace* ~[\r\n]*
        // -> skip
    ;

Whitespace
    :   [ \t]+
        // -> skip
    ;

// 换行符号，可以利用这个信息来更新行号
Newline
    :   (   '\r' '\n'?
        |   '\n'
        )
        // -> skip
    ;

