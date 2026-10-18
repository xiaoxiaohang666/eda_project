// Standalone, non-CTest comparison. Build WITHOUT ASan: the untouched oracle
// method leaks its returned tree; it runs only in a separate exec'd process.
// Example from the project root:
// g++ -std=c++17 -O2 -I src tests/native_flute_parity_probe.cpp src/objectives/official_metrics.cpp build/libeda_core.a -ldl -o OUTPUT
// OUTPUT output/runtime/official-flute
#include "objectives/official_metrics.hpp"
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <vector>
using namespace eda;
namespace fs=std::filesystem;
namespace {
using Case=std::vector<Point>;
std::vector<Case> cases() {
    std::vector<Case> result{
        {{0,0},{10,0}},{{0,0},{10,10}},{{0,0},{0,0}},
        {{0,0},{0,0},{0,0}},{{0,0},{0,0},{10,0}},
        {{0,0},{10,0},{10,10},{20,10}},
        Case(150,Point{0,0}),{{-10,-20},{30,40}},
        {{0,0},{16777217,0},{16777217,11},{33554433,11}}
    };
    std::mt19937 generator(271828);
    std::uniform_int_distribution<int> coordinate(-1000000,1000000);
    for(int degree=2;degree<=150;++degree) {
        Case points;
        for(int i=0;i<degree;++i) {
            if(i>0 && i%7==0)points.push_back(points.front());
            else points.push_back({double(coordinate(generator)),double(coordinate(generator))});
        }
        result.push_back(std::move(points));
    }
    return result;
}
void require(bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}
void* load(const fs::path& path,int flags) {
    auto handle=dlopen(path.c_str(),flags);
    if(!handle)throw std::runtime_error(dlerror());
    return handle;
}
int original_oracle(const fs::path& assets) {
#if defined(__SANITIZE_ADDRESS__)
    throw std::runtime_error("the original leaking oracle must run without ASan");
#endif
    struct rlimit no_core{0,0};setrlimit(RLIMIT_CORE,&no_core);
    fs::current_path(assets);
    void* python=load("libpython3.10.so.1.0",RTLD_NOW|RTLD_GLOBAL);
    void* flute=load(assets/"libflute.so",RTLD_NOW|RTLD_GLOBAL);
    void* extension=load(assets/"tse_framework_pybind_flute.cpython-310-x86_64-linux-gnu.so",RTLD_NOW|RTLD_LOCAL);
    using Ctor=void(*)(void*);
    using Fn=std::tuple<float,int>(*)(void*,int,const std::vector<int>&,const std::vector<int>&,bool);
    auto ctor=reinterpret_cast<Ctor>(dlsym(extension,"_ZN3tse9framework12SteinerFluteC1Ev"));
    auto original=reinterpret_cast<Fn>(dlsym(extension,"_ZN3tse9framework12SteinerFlute16GetSteinerLengthEiRKSt6vectorIiSaIiEES6_b"));
    require(ctor && original,"original official ABI missing");
    alignas(16) std::array<unsigned char,16> object{};ctor(object.data());
    const auto inputs=cases();
    std::cout<<std::setprecision(std::numeric_limits<float>::max_digits10);
    for(size_t i=0;i<inputs.size();++i) {
        std::vector<int> x(150),y(150);
        for(size_t j=0;j<inputs[i].size();++j) {x[j]=int(inputs[i][j].x);y[j]=int(inputs[i][j].y);}
        const auto [length,vias]=original(object.data(),int(inputs[i].size()),x,y,false);
        std::cout<<"ORACLE "<<i<<" "<<length<<" "<<vias<<std::endl;
    }
    const std::vector<int> padding(150);
    const auto [high_length,high_vias]=original(object.data(),151,padding,padding,false);
    require(high_length==-1 && high_vias==-1,"original degree limit boundary");
    std::cout<<"ORACLE_HIGH_DEGREE -1 -1"<<std::endl;
    // Do not call degree 1: the untouched wrapper's native flute call crashes.
    dlclose(extension);dlclose(flute);dlclose(python);
    return 0;
}
std::map<size_t,std::pair<float,int>> read_oracle(const fs::path& executable,const fs::path& assets) {
    int channel[2];require(pipe(channel)==0,"oracle pipe failed");
    const auto child=fork();require(child>=0,"oracle fork failed");
    if(child==0) {
        close(channel[0]);dup2(channel[1],STDOUT_FILENO);close(channel[1]);
        execl(executable.c_str(),executable.c_str(),"--oracle",assets.c_str(),nullptr);
        _exit(127);
    }
    close(channel[1]);
    FILE* source=fdopen(channel[0],"r");require(source,"oracle fdopen failed");
    std::map<size_t,std::pair<float,int>> expected;
    char* line=nullptr;size_t capacity=0;bool saw_high_degree=false;
    while(getline(&line,&capacity,source)>=0) {
        const std::string text(line);
        const auto marker=text.find("ORACLE ");
        if(marker!=std::string::npos) {
            std::istringstream fields(text.substr(marker+7));size_t index;float length;int vias;
            require(bool(fields>>index>>length>>vias),"invalid oracle record");
            require(expected.emplace(index,std::make_pair(length,vias)).second,"duplicate oracle record");
        }
        if(text.find("ORACLE_HIGH_DEGREE -1 -1")!=std::string::npos)saw_high_degree=true;
    }
    std::free(line);fclose(source);
    int status=0;require(waitpid(child,&status,0)==child,"oracle wait failed");
    require(WIFEXITED(status) && WEXITSTATUS(status)==0,"original oracle process failed");
    require(saw_high_degree,"original oracle high-degree guard missing");
    return expected;
}
}
int main(int argc,char**argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--oracle")
            return original_oracle(fs::absolute(argv[2]));
        require(argc==2,"native_flute_parity_probe OFFICIAL_RUNTIME");
        const auto assets=fs::absolute(argv[1]);
        const auto expected=read_oracle(fs::canonical(argv[0]),assets);
        const auto inputs=cases();require(expected.size()==inputs.size(),"oracle case count mismatch");
        NativeFluteBaseline native(assets.string());require(native.max_degree()==150,"native degree limit");
        for(size_t i=0;i<inputs.size();++i) {
            const auto actual=native.evaluate_points(inputs[i]);const auto reference=expected.at(i);
            require(!actual.skipped_degree_limit && actual.wirelength_dbu==double(reference.first) &&
                    actual.via_count==reference.second,"exact native/original mismatch at case "+std::to_string(i));
        }
        const auto singleton=native.evaluate_points({{5,7}});
        require(singleton.wirelength_dbu==0 && singleton.via_count==0 && !singleton.skipped_degree_limit,
                "single-point safe boundary");
        const auto high=native.evaluate_points(Case(151));
        require(high.skipped_degree_limit && high.wirelength_dbu==0 && high.via_count==0,"high-degree skip");
        bool rejected=false;
        try {native.evaluate_points({{double(INT_MIN),0},{double(INT_MAX),0}});}
        catch(const std::runtime_error&){rejected=true;}
        require(rejected,"native overflowing coordinate span must be rejected");
        // Repeat owned native calls; an ASan production test can use this same
        // coverage without ever executing the original leaking oracle method.
        for(int repeat=0;repeat<1000;++repeat)
            require(native.evaluate_points({{0,0},{10,0}}).wirelength_dbu==10,"repeated owned degree-2 calls");
        std::cout<<"native_flute_parity_probe: "<<inputs.size()
                 <<" exact degree2..150 oracle comparisons PASS\n"
                 <<"duplicate/negative coordinates and float rounding included; high-degree skip PASS\n"
                 <<"single-point safe zero PASS (original wrapper crashes; not numeric parity)\n"
                 <<"INT_MAX span rejection and 1000 owned calls PASS\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"native_flute_parity_probe: "<<error.what()<<"\n";return 1;
    }
}
