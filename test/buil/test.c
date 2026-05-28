int add(int a, int b) {
    return a + b;
}
__attribute__((annotate("shared")))
int x=2;
void hello() ;

__attribute__((annotate("vcpus:2"),annotate("hello:world")))
int fureh(){
    return 0;
}