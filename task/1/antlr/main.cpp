#include "SYsULexer.h" // 确保这里的头文件名与您生成的词法分析器匹配
#include <fstream>
#include <iostream>
#include <unordered_map>

struct LineMarker {
    int lineNumber;
    std::string filename;
    int flags[4];
};

LineMarker parseLineMark(const std::string& line) {
    LineMarker marker;
    // 初始化 flags 为 0
    for (int i = 0; i < 4; ++i) marker.flags[i] = 0;
    
    size_t pos = 0;
    // 跳过前导空格
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    // 检查 '#'
    if (pos >= line.size() || line[pos] != '#') {
        // 格式错误，返回默认值
        marker.lineNumber = 0;
        marker.filename = "";
        return marker;
    }
    ++pos; // 跳过 '#'
    // 跳过空格
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    // 解析行号
    int lineNumber = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        lineNumber = lineNumber * 10 + (line[pos] - '0');
        ++pos;
    }
    marker.lineNumber = lineNumber;
    // 跳过空格
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    // 解析文件名
    std::string filename;
    if (pos < line.size() && line[pos] == '"') {
        // 带引号的文件名
        ++pos; // 跳过开头的引号
        size_t start = pos;
        while (pos < line.size() && line[pos] != '"') ++pos;
        filename = line.substr(start, pos - start);
        if (pos < line.size()) ++pos; // 跳过结尾的引号
    } else {
        // 不带引号的文件名（理论上不存在，但处理一下）
        size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
        filename = line.substr(start, pos - start);
    }
    marker.filename = filename;
    // 跳过空格
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    // 解析 flags
    int flagIndex = 0;
    while (pos < line.size() && flagIndex < 4) {
        // 解析整数
        int flag = 0;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            flag = flag * 10 + (line[pos] - '0');
            ++pos;
        }
        marker.flags[flagIndex++] = flag;
        // 跳过空格
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    }
    return marker;
}

// 映射定义，将ANTLR的tokenTypeName映射到clang的格式
std::unordered_map<std::string, std::string> tokenTypeMapping = {
  // { "Int", "int" },
  { "Identifier", "identifier" },
  // { "LeftParen", "l_paren" },
  // { "RightParen", "r_paren" },
  // { "RightBrace", "r_brace" },
  // { "LeftBrace", "l_brace" },
  { "LeftBracket", "l_square" },
  { "RightBracket", "r_square" },
  { "Constant", "numeric_constant" },
  // { "Return", "return" },
  // { "Semi", "semi" },
  { "EOF", "eof" },
  // { "Equal", "equal" },
  // { "Plus", "plus" },
  // { "Comma", "comma" },

  // 在这里继续添加其他映射
  { "Char", "char" },
  { "Short", "short" },
  { "Int", "int" },
  { "Long", "long" },
  { "Float", "float" },
  { "Double", "double" },
  { "Void", "void" },
  { "Signed", "signed" },
  { "Unsigned", "unsigned" },
  { "Struct", "struct" },
  { "Union", "union" },
  { "Enum", "enum" },
  { "Typedef", "typedef" },
  { "Extern", "extern" },
  { "Static", "static" },
  { "Auto", "auto" },
  { "Register", "register" },
  { "Const", "const" },
  { "Volatile", "volatile" },
  { "Restrict", "restrict" },
  { "Inline", "inline" },
  { "If", "if" },
  { "Else", "else" },
  { "While", "while" },
  { "Do", "do" },
  { "For", "for" },
  { "Switch", "switch" },
  { "Case", "case" },
  { "Default", "default" },
  { "Break", "break" },
  { "Continue", "continue" },
  { "Return", "return" },
  { "Goto", "goto" },
  { "Sizeof", "sizeof" },
  { "LeftParen", "l_paren" },
  { "RightParen", "r_paren" },
  { "LeftBrace", "l_brace" },
  { "RightBrace", "r_brace" },
  { "Plus", "plus" },
  { "Minus", "minus" },
  { "Star", "star" },
  { "Slash", "slash" },
  { "Percent", "percent" },
  { "Amp", "amp" },
  { "Pipe", "pipe" },
  { "Caret", "caret" },
  { "Tilde", "tilde" },
  { "Exclaim", "exclaim" },
  { "Less", "less" },
  { "Greater", "greater" },
  { "Equal", "equal" },
  { "Question", "question" },
  { "Colon", "colon" },
  { "Semi", "semi" },
  { "Comma", "comma" },
  { "Period", "period" },
  { "Arrow", "arrow" },
  { "PlusPlus", "plusplus" },
  { "MinusMinus", "minusminus" },
  { "LessLess", "lessless" },
  { "GreaterGreater", "greatergreater" },
  { "LessLessEqual", "lesslessequal" },
  { "GreaterGreaterEqual", "greatergreaterequal" },
  { "AmpAmp", "ampamp" },
  { "PipePipe", "pipepipe" },
  { "EqualEqual", "equalequal" },
  { "ExclaimEqual", "exclaimequal" },
  { "LessEqual", "lessequal" },
  { "GreaterEqual", "greaterequal" },
  { "PlusEqual", "plusequal" },
  { "MinusEqual", "minusequal" },
  { "StarEqual", "starequal" },
  { "SlashEqual", "slashequal" },
  { "PercentEqual", "percentequal" },
  { "AmpEqual", "ampequal" },
  { "PipeEqual", "pipeequal" },
  { "CaretEqual", "caretequal" },
  { "Ellipsis", "ellipsis" }
};

void
print_token(const antlr4::Token* token,
            const antlr4::CommonTokenStream& tokens,
            std::ofstream& outFile,
            const antlr4::Lexer& lexer)
{
  auto& vocabulary = lexer.getVocabulary();

  auto tokenTypeName =
    std::string(vocabulary.getSymbolicName(token->getType()));

  if (tokenTypeName.empty())
    tokenTypeName = "<UNKNOWN>"; // 处理可能的空字符串情况

  if (tokenTypeMapping.find(tokenTypeName) != tokenTypeMapping.end()) {
    tokenTypeName = tokenTypeMapping[tokenTypeName];
  }

  static LineMarker lastLineMark;
  static size_t lineNumber = 0;
  if (tokenTypeName == "LineAfterPreprocessing") {
    lastLineMark = parseLineMark(token->getText());
    lineNumber = lastLineMark.lineNumber - 1;
    return;
  }
  
  std::string locInfo = " Loc=<" + lastLineMark.filename + ":" +
                          std::to_string(lineNumber) + ":" +
                          std::to_string(token->getCharPositionInLine() + 1) +
                          ">";

  static bool startOfLine = false;
  static bool leadingSpace = false;

  if (tokenTypeName == "Whitespace") {
    leadingSpace = true;
    return;
  }
  if (tokenTypeName == "Newline") {
    startOfLine = true;
    ++ lineNumber;
    return;
  }

  if (token->getText() != "<EOF>")
    outFile << tokenTypeName << " '" << token->getText() << "'";
  else
    outFile << tokenTypeName << " '"
            << "'";
  if (startOfLine) {
    outFile << "\t [StartOfLine]";
    startOfLine = false;
  }
  if (leadingSpace) {
    outFile << " [LeadingSpace]";
    leadingSpace = false;
  }
  outFile << locInfo << std::endl;
}

int
main(int argc, char* argv[])
{
  if (argc != 3) {
    std::cout << "Usage: " << argv[0] << " <input> <output>\n";
    return -1;
  }

  std::ifstream inFile(argv[1]);
  if (!inFile) {
    std::cout << "Error: unable to open input file: " << argv[1] << '\n';
    return -2;
  }

  std::ofstream outFile(argv[2]);
  if (!outFile) {
    std::cout << "Error: unable to open output file: " << argv[2] << '\n';
    return -3;
  }

  std::cout << "程序 '" << argv[0] << std::endl;
  std::cout << "输入 '" << argv[1] << std::endl;
  std::cout << "输出 '" << argv[2] << std::endl;

  antlr4::ANTLRInputStream input(inFile);
  SYsULexer lexer(&input);

  antlr4::CommonTokenStream tokens(&lexer);
  tokens.fill();

  for (auto&& token : tokens.getTokens()) {
    print_token(token, tokens, outFile, lexer);
  }
}
