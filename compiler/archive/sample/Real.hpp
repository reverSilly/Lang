#pragma once
#include"Token.hpp"
#include"Tag.hpp"
#include<string>
namespace reverSilly
{
	namespace lexer
	{
		class Real:Token
		{
		public:
			const float value;
			Real(float value)
			:Token(Tag::REAL),value{value}{}
			std::string toString() const override
			{
				return std::to_string(value);
			}
		};
	}
}
