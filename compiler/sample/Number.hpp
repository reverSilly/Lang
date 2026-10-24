#pragma once
#include"Tag.hpp"
#include"Token.hpp"
#include<string>
#include<functional>
namespace reverSilly
{
	namespace lexer
	{
		class Number:Token
		{
		public:
			const int value;
			Number(int value)
			:Token(Tag::NUM),value{value}{}
			std::string toString() const override
			{
				return std::to_string(value);
			}
		};
	}
}
