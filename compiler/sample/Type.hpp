#pragma once
#include"Word.hpp"
#include"Tag.hpp"
#include<optional>
#include<string>
namespace reverSilly
{
	namespace symbles
	{
		class Type:reverSilly::lexer::Word
		{
		public:
			size_t width;
			Type(std::string str,Tag tag,size_t width)
			:Word(str,tag),width{width}{}
			const static Type Integer,Float,Char,Bool;
			bool operator ==(const Type&)const=default;
			friend bool numeric(const Type& b)
			{
				return b==Integer||b==Float;
			}
			friend bool integeral(const Type& b)
			{
				return b==Integer||b==Char;
			}
		};
		const Type Type::Integer("integer",Tag::BASIC,4);
		const Type Type::Float("float",Tag::BASIC,8);
		const Type Type::Char("char",Tag::BASIC,1);
		const Type Type::Bool("bool",Tag::BASIC,1);
	}
}
