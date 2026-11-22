//
// Created by Louis Li on 2023/8/11.
//

#ifndef COMPILER_PARSER_H
#define COMPILER_PARSER_H
//
// Created by Louis Li on 2022/7/22.
//
#include "peg_parser/generator.h"
#include "Expression.h"
#include <memory>
#include <string_view>
#include <unordered_map>

namespace reverSilly
{
	namespace Lang
	{
		using namespace peg_parser;
		using namespace std;

		namespace Parse {
			using namespace SyntaxTree;
			[[nodiscard("Never gonna give the result up~")]]
			inline ParserGenerator<unique_ptr<Expression>, Object *> GetParser
				(const unordered_map<string_view, typename peg_parser::Interpreter<unique_ptr<Expression>, Object *>::Callback> &callbacks = {});
			extern unordered_map<string_view,typename peg_parser::Interpreter<unique_ptr<Expression>,Object*>::Callback>DefaultCallbacks;
		}
	}
}
//Suffixes(Call(Access(Unit(Chars(Char('1'),),),),),Pioneer('<'),Call(Access(Unit(Null(''),),),),)
#endif //COMPILER_PARSER_H
