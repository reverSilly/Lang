#pragma once

#include <stdexcept>
#include <exception>
#include <string_view>
#include <vector>

#include "grammar.h"

namespace peg_parser
{
	struct SyntaxTree
	{
		std::shared_ptr<grammar::Rule>rule;
		std::basic_string_view<unsigned char> fullString;
		std::vector<std::shared_ptr<SyntaxTree>>inner;
		size_t begin,end;
		
		bool valid=false;
		bool active=true;
		bool recursive=false;
		
		SyntaxTree(const std::shared_ptr<grammar::Rule> &r,std::basic_string_view<unsigned char> s,size_t p);
		SyntaxTree(const std::shared_ptr<grammar::Rule> &r,std::basic_string_view<char> s,size_t p);
		
		size_t length() const
		{
			return end-begin;
		}
		std::basic_string_view<unsigned char> view() const
		{
			return fullString.substr(begin,length());
		}
		std::basic_string<unsigned char>string() const
		{
			return std::basic_string<unsigned char>(view());
		}
		std::string_view cview() const
		{
			return std::string_view(reinterpret_cast<char const*>(fullString.begin())+begin,length());
		}
		std::string cstring() const
		{
			return std::string(cview());
		}
	}; //struct SyntaxTree
	
	struct Parser
	{
		struct Result
		{
			std::shared_ptr<SyntaxTree>syntax;
			std::shared_ptr<SyntaxTree>error;
		};
		
		struct GrammarError : std::exception
		{
			enum class Type :unsigned char
			{
				UnKnownSymbol,
				InvalidRule
			} type;
			grammar::Node::Shared node;
			mutable std::string buffer;
			GrammarError(Type t, grammar::Node::Shared n)
			:type(t),node(n){}
			const char *what()const noexcept override;
		};
		
		std::shared_ptr<grammar::Rule> grammar;
		
		Parser
		(
			const std::shared_ptr<grammar::Rule> &grammar
			=std::make_shared<grammar::Rule>("undefined",grammar::Node::Error())
			);
		
		static Result parseAndGetError
		(
			std::basic_string_view<char> str,
			const std::shared_ptr<grammar::Rule>& grammar
		);
		static Result parseAndGetError
		(
			std::basic_string_view<unsigned char> str,
			const std::shared_ptr<grammar::Rule>& grammar
		);
		static std::shared_ptr<SyntaxTree> parse
		(
			std::basic_string_view<char> str,
			const std::shared_ptr<grammar::Rule>& grammar
		);
		static std::shared_ptr<SyntaxTree> parse
		(
			std::basic_string_view<unsigned char> str,
			const std::shared_ptr<grammar::Rule>& grammar
		);
		
		std::shared_ptr<SyntaxTree>	parse(std::basic_string_view<char> str) const;
		std::shared_ptr<SyntaxTree>	parse(std::basic_string_view<unsigned char> str) const;
		Result parseAndGetError(std::basic_string_view<char> str) const;
		Result parseAndGetError(std::basic_string_view<unsigned char> str) const;
	}; //class Parser
	
	template<typename Letter>
	std::basic_ostream<Letter> &operator<<(std::basic_ostream<Letter> &stream, const SyntaxTree &tree)
	{
		stream<<tree.rule->name<<'(';
		if (tree.inner.empty())
			stream<<'\''<< tree.cview()<<'\'';
		else
		{
			for(auto &&i:tree.inner)
				stream<<*i<<',';
		}
		stream<<')';
		return stream;
	}
	
}  // namespace peg_parser
