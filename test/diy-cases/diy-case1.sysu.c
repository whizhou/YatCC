int main() {
    int a = 5;
    int b = 10;
    int c = 15;
    int result;

    // 综合运算符的长表达式
    result = (a + -b * (c % 4) + (a != b)) > ((c * 1) && (a != b));

    return 0;
}