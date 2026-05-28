#include<iostream>
#include<string>
class Person{
    public:
    std::string first;
    std::string last;

    void printName(){
        std::cout<<first<<" and "<<last<<std::endl;
    }
};
int main(){
    Person p;
    p.first="Caleb";
    p.last="fureh";
    p.printName();
}