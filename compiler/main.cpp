#include <iostream>
#include "peg_parser/generator.h"
#include "Expression.h"
#include "Parser.cpp"

using namespace std;
using namespace peg_parser;
using namespace reverSilly::Lang;
int main()
{
	auto psr=GetParser();
	string input;
	while(getline(cin,input))
	{
		try
		{
			psr.run(input,nullptr);
		}
		catch(InterpreterError& err)
		{
			cout<<"!OK"<<endl<<err.buffer<<endl;
		}
		catch(SyntaxError& err)
		{
			cout<<*err.syntax<<endl;
		}
	}
	return 0;
}
