parser grammar SYsUParser;

options {
  tokenVocab=SYsULexer;
}

primaryExpression
    :   Identifier
    |   Constant
    |   LeftParen expression RightParen
    ;

postfixExpression
    :   primaryExpression
    |   postfixExpression LeftBracket expression RightBracket
    |   postfixExpression LeftParen RightParen
    |   postfixExpression LeftParen argumentExpressionList RightParen
    ;

argumentExpressionList
    :   assignmentExpression (Comma assignmentExpression)*
    ;

unaryExpression
    :
    (postfixExpression
    |   unaryOperator unaryExpression
    )
    ;

unaryOperator
    :   Plus | Minus | Not
    ;

multiplicativeExpression
    :   unaryExpression ((Star|Slash|Percent) unaryExpression)*
    ;

additiveExpression
    :   multiplicativeExpression ((Plus|Minus) multiplicativeExpression)*
    ;

relationalExpression
    :   additiveExpression ((Less | Greater | LessEqual | GreaterEqual) additiveExpression)*
    ;

equalityExpression
    :   relationalExpression ((EqualEqual | NotEqual) relationalExpression)*
    ;

logicalAndExpression
    :   equalityExpression (And equalityExpression)*
    ;

logicalOrExpression
    :   logicalAndExpression (Or logicalAndExpression)*
    ;

assignmentExpression
    :   logicalOrExpression
    |   unaryExpression Equal assignmentExpression
    ;

expression
    :   assignmentExpression (Comma assignmentExpression)*
    ;


declaration
    :   declarationSpecifiers initDeclaratorList? Semi
    ;

declarationSpecifiers
    :   declarationSpecifier+
    ;

declarationSpecifier
    :   typeSpecifier
    |   typeQualifier
    ;

initDeclaratorList
    :   initDeclarator (Comma initDeclarator)*
    ;

initDeclarator
    :   declarator (Equal initializer)?
    ;


typeSpecifier
    :   Int
    |   Void
    ;


typeQualifier
    :   Const
    ;


declarator
    :   directDeclarator
    ;

directDeclarator
    :   Identifier
    |   directDeclarator LeftBracket assignmentExpression RightBracket
    |   directDeclarator LeftBracket RightBracket
    |   directDeclarator LeftParen parameterTypeList RightParen
    |   directDeclarator LeftParen identifierList RightParen
    |   directDeclarator LeftParen RightParen
    ;

parameterTypeList
    :   parameterList
    // |   parameterTypeList Comma Ellipsis
    ;

parameterList
    :   parameterDeclaration
    |   parameterList Comma parameterDeclaration
    ;

parameterDeclaration
    :   declarationSpecifiers declarator
    // |   declarationSpecifiers abstractDeclarator
    |   declarationSpecifiers
    ;

identifierList
    :   Identifier (Comma Identifier)*
    ;

initializer
    :   assignmentExpression
    |   LeftBrace initializerList? Comma? RightBrace
    ;

initializerList
    // :   designation? initializer (Comma designation? initializer)*
    :   initializer (Comma initializer)*
    ;

statement
    :   compoundStatement
    |   expressionStatement
    |   selectionStatement
    |   iterationStatement
    |   jumpStatement
    ;

compoundStatement
    :   LeftBrace blockItemList? RightBrace
    ;

blockItemList
    :   blockItem+
    ;

blockItem
    :   statement
    |   declaration
    ;

expressionStatement
    :   expression? Semi
    ;

selectionStatement
    :   If LeftParen expression RightParen statement (Else statement)?
    ;

iterationStatement
    :   While LeftParen expression RightParen statement
    ;

jumpStatement
    :   Return expression? Semi
    |   Break Semi
    |   Continue Semi
    ;

compilationUnit
    :   translationUnit? EOF
    ;

translationUnit
    :   externalDeclaration+
    ;

externalDeclaration
    :   functionDefinition
    |   declaration
    ;

functionDefinition
    : declarationSpecifiers directDeclarator declarationList compoundStatement
    | declarationSpecifiers directDeclarator compoundStatement
    ;

declarationList
    : declaration
    | declarationList declaration
    ;

