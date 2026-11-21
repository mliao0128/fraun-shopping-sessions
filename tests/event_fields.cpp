#include "../event_fields.hpp"
#include <random>
#include <iostream>
#include <fstream>
int main(int argc, char** argv) {
    std::mt19937 rng(93528);
    std::array<std::string_view,7> a,b;
    for (size_t i=0;i<500000;++i) {
        std::string s;
        for (unsigned column=0;column<7;++column) {
            if(column)s+=',';
            const bool quoted = rng()%2;
            if(quoted)s+='"';
            size_t choices[] = {9,9,13u+(rng()%3)*5,20,4,2u+rng()%7,rng()%9};
            size_t n=(rng()%17==0) ? rng()%48 : choices[column];
            for (size_t k=0;k<n;++k) {
                unsigned char ch;
                do ch=rng()%256;while(ch=='\n'||ch==','||ch=='"');
                s+=static_cast<char>(ch);
            }
            if(quoted)s+='"';
        }
        if(i%3==0 && !s.empty())s[rng()%s.size()]=(rng()&1)?'"':',';
        if(i%13==0)s.resize(rng()%110,',');
        bool expected=csvGeneric(s,a),actual=eventFields(s,b);
        if(expected!=actual || (expected && a!=b))return 1;
    }
    size_t checked = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i]);
        if (!input) return 2;
        std::string line;
        while (std::getline(input,line)) {
            bool expected = csvGeneric(line,a), actual = eventFields(line,b);
            if (expected != actual || (expected && a != b)) return 1;
            ++checked;
        }
    }
    std::cout << checked << " real input lines matched; ";
    std::cout<<"500000 randomized mixed-quote event rows matched csvGeneric\n";
}
