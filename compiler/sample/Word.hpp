#pragma once
#include"Tag.hpp"
#include"Token.hpp"
#include<string>
#include<string_view>
namespace reverSilly
{
	namespace lexer
	{
		class Word:Token
		{
		public:
			std::string lexeme;
			Word(std::string s,Tag tag)
			:Token(tag),lexeme{s}{}
			std::string toString() const override
			{
				return lexeme;
			}
			static const Word And,Equal,LeftEqual,Minus,True,False,Temporary;
			virtual bool operator ==(const Word&)const=default;
		};
		const Word Word::And("&&",Tag::AND);
		const Word Word::Equal("==",Tag::EQUAL);
		const Word Word::LeftEqual("<=",Tag::LESSEQUAL);
		const Word Word::Minus("minus",Tag::MINUS);
		const Word Word::True("true",Tag::TRUE);
		const Word Word::False("false",Tag::FALSE);
		const Word Word::Temporary("t",Tag::TEMPORARY);
	}
}
