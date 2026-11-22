#pragma once
#include"Tag.hpp"
#include<string>
namespace reverSilly
{
	namespace lexer
	{
		class Token
		{
		public:
			const Tag tag;
			Token(Tag tag)
			:tag(tag){}
			virtual std::string toString() const
			{
				return std::string(static_cast<char>(tag),1);
			}
			virtual bool operator ==(const Token&)const=default;
		};
	}
}
