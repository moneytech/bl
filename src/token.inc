sm(EOF = 0,        "end")
sm(IDENT,          "identifier")
sm(STRING,         "string")
sm(CHAR,           "char")
sm(NUM,            "number")
sm(FLOAT,          "float")
sm(DOUBLE,         "double")
sm(IF,             "if") /* must be first */
sm(FN,             "fn")
sm(CAST,           "cast")
sm(CAST_AUTO,      "auto")
sm(ELSE,           "else")
sm(TRUE,           "true")
sm(NULL,           "null")
sm(ENUM,           "enum")
sm(LOOP,           "loop")
sm(FALSE,          "false")
sm(DEFER,          "defer")
sm(BREAK,          "break")
sm(SWITCH,         "switch")
sm(RETURN,         "return")
sm(SIZEOF,         "sizeof")
sm(STRUCT,         "struct")
sm(DEFAULT,        "default")
sm(ALIGNOF,        "alignof")
sm(CONTINUE,       "continue")
sm(TYPEINFO,       "typeinfo")
sm(UNREACHABLE,    "unreachable")
sm(AT,             "@")
sm(LCOMMENT,       "//")
sm(LBCOMMENT,      "/*")
sm(RBCOMMENT,      "*/")
sm(HASH,           "#")
sm(LBLOCK,         "{")
sm(RBLOCK,         "}")
sm(LBRACKET,       "[")
sm(RBRACKET,       "]")
sm(LPAREN,         "(")
sm(RPAREN,         ")")
sm(COMMA,          ",")
sm(SEMICOLON,      ";")
sm(COLON,          ":")
sm(VARGS,          "...")
sm(EQ,             "==") /* logical begin */
sm(NEQ,            "!=")
sm(GREATER_EQ,     ">=")
sm(LESS_EQ,        "<=")
sm(PLUS_ASSIGN,    "+=")
sm(MINUS_ASSIGN,   "-=")
sm(ASTERISK_ASSIGN,"*=")
sm(SLASH_ASSIGN,   "/=")
sm(PERCENT_ASSIGN, "%=")
sm(LOGIC_AND,      "&&")
sm(SHR,            ">>")
sm(SHL,            "<<")
sm(LOGIC_OR,       "||")
sm(GREATER,        ">")
sm(LESS,           "<")
sm(ASSIGN,         "=")
sm(AND,            "&")
sm(OR,             "|")
sm(CARET,          "^")
sm(NOT,            "!")
sm(SLASH,          "/")
sm(PERCENT,        "%")
sm(PLUS,           "+")
sm(MINUS,          "-")
sm(ASTERISK,       "*") /* logical end */
sm(DOT,            ".")
sm(NONE,           "")