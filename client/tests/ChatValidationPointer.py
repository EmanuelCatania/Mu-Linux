#!/usr/bin/env python3
"""Validador de nombres (personaje y guild): caracteres y nombres reservados."""
import argparse
import os
from pathlib import Path
import re
import tempfile
from GatePointer import PREAMBLE
from SceneObjectPointer import build_and_run, function, read_source

SERVICES=r'''
#include <string>
using uint=unsigned;using byte=BYTE;
// Desde la 0.97.50 el validador no usa las listas de palabras (FilterName.bmd
// y Filter.bmd): sólo los caracteres y los nombres reservados.
BYTE *inputMemory;char *input;
#define InputText (*reinterpret_cast<char(*)[10][256]>(input))
char GlobalText[459][300];
ALIASES
CONSTANTS
CONCAT
std::string expectedRaw;std::vector<unsigned> calls;
int mbclen(byte* p){Require(*p>0 && *p<128);return 1;}
FIND
char* Block(unsigned n){
 char* values[]={(char*)&DAT_00561740,(char*)&DAT_00561744,(char*)&DAT_00561748,(char*)&DAT_0056174c,(char*)&DAT_00561750,(char*)"Webzen",(char*)"WebZen",(char*)"webzen",(char*)"WEBZEN",&DAT_07d4b4b0,&DAT_07d4b5dc};
 Require(n<11);return values[n];
}
bool FindTextA(char* text,char* pattern,bool sensitive){
 unsigned id=0;
 for(unsigned i=0;i<11;++i)if(pattern==Block(i)){id=4000+i;break;}
 // Los cuatro patrones de Webzen son literales: se reconocen por contenido.
 if(!id)for(unsigned i=5;i<9;++i)if(std::strcmp(pattern,Block(i))==0){id=4000+i;break;}
 Require(id && !sensitive && std::strcmp(text,expectedRaw.c_str())==0);
 calls.push_back(id);return ActualFind(text,pattern,sensitive);
}
'''
MAIN=r'''
bool Contains(const std::string& text,const char* pattern){return *pattern && text.find(pattern)!=std::string::npos;}
std::uint64_t Run(BYTE* n){
 inputMemory=n;
 const std::string texts[]={"","alpha","/alpha","a l p h a"," beta ","omega","Webzen","WEBZEN",".","/cmd","none","  ","ab cd","slash/",std::string(255,'x'),"/"+std::string(254,'x')};
 std::uint64_t hash=14695981039346656037ull;unsigned queries=0;
 for(unsigned offset: {0u,31u})for(unsigned pattern=0;pattern<2;++pattern)for(auto& text:texts){
  std::memset(n,0xa5,2624);input=reinterpret_cast<char*>(n+16+offset);std::memcpy(input,text.c_str(),text.size()+1);
  std::memset(GlobalText[457],0,600);if(pattern){std::strcpy(GlobalText[457],"omega");std::strcpy(GlobalText[458],"cmd");}
  expectedRaw=text;
  const std::vector<BYTE> beforeInput(n,n+2624);
  char beforeGlobal[600];std::memcpy(beforeGlobal,GlobalText[457],600);
  std::vector<unsigned> expected;bool blocked=false;
  for(unsigned i=0;i<11;++i){expected.push_back(4000+i);if(Contains(expectedRaw,Block(i))){blocked=true;break;}}
  calls.clear();const char result=Chat_ValidateInputCommand();
  Require(result==char(blocked) && calls==expected && std::memcmp(n,beforeInput.data(),2624)==0 && std::memcmp(GlobalText[457],beforeGlobal,600)==0);
  hash^=static_cast<BYTE>(result);hash*=1099511628211ull;for(auto id:calls){hash^=id;hash*=1099511628211ull;}++queries;
 }
 Require(queries==64);return hash;
}
int main(){
 auto* low=static_cast<BYTE*>(Reserve(0x26000000u));Require(low);const auto expected=Run(low);
 if(sizeof(void*)>4){
  auto* high=static_cast<BYTE*>(Reserve(std::uint64_t(0x126000000ull)));Require(high);
  std::memset(low,0x7b,0x10000);
  Require(Run(high)==expected);
  for(unsigned i=0;i<2624;++i)Require(low[i]==0x7b);
  std::puts("texto alto: mismos patrones y resultado");
 }
 std::printf("Chat validation: 64 consultas por ruta %016llx\n",static_cast<unsigned long long>(expected));
}
'''
def harness(revision=None):
    globals_source=read_source('src/globals.cpp',revision)
    header=read_source('src/globals.h',revision)
    names=('DAT_00561740','DAT_00561744','DAT_00561748','DAT_0056174c','DAT_00561750',)
    constants='\n'.join(re.search(r'^(?:DWORD|char)\s+'+name+r'\s*(?:\[\])?\s*=\s*[^;]+;',globals_source,re.M).group(0) for name in names)
    aliases='\n'.join(re.search(r'^#define '+name+r'\s+[^\n]+',header,re.M).group(0) for name in ('DAT_07d4b4b0','DAT_07d4b5dc'))
    concat=re.search(r'^#define CONCAT31[^\n]+',read_source('src/types.h',revision),re.M).group(0)
    find=function(read_source('src/ui/UI_LegacyExterns.cpp',revision),'FindTextA').replace('FindTextA','ActualFind')
    services=SERVICES.replace('ALIASES',aliases).replace('CONSTANTS',constants).replace('CONCAT',concat).replace('FIND',find)
    return PREAMBLE.split('char DAT_00559bb4',1)[0].replace('0x10000','0x40000')+services+function(read_source('src/ui/Chat.cpp',revision),'Chat_ValidateInputCommand')+MAIN

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--architecture',choices=('Win32','x64'),default='x64')
    parser.add_argument('--generator')
    parser.add_argument('--reference-ref')
    args=parser.parse_args()
    if args.reference_ref and (os.name!='nt' or args.architecture!='Win32'):parser.error('Referencia sólo en Windows x86')
    with tempfile.TemporaryDirectory(prefix='mu-chat-validation-') as temp:
        directory=Path(temp);current=build_and_run(directory/'current',harness(),args);print(current,end='')
        if args.reference_ref:
            reference=build_and_run(directory/'reference',harness(args.reference_ref),args)
            if current!=reference:raise RuntimeError('Diferencia con referencia x86')
            print(f'Equivalencia con {args.reference_ref}: OK')
if __name__=='__main__':main()
