#pragma once

#include <functional>
#include <memory>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <list>
#include <utility>
#include <bit>
#include "easy_iterator.h"

namespace peg_parser
{
	struct SyntaxTree;
	
	namespace grammar
	{
		struct Node;
		
		struct Rule
		{
			std::string name;
			std::shared_ptr<Node> node;
			bool hidden;
			bool cacheable;
			Rule
			(
				std::string_view n,
				const std::shared_ptr<Node> &t,
				bool hidden=false,
				bool cacheable=true
				)
			:name(n),node(t),hidden{hidden},cacheable{cacheable}{}
		};
		
		inline std::shared_ptr<Rule>
		makeRule
		(
			std::string_view name,
			const std::shared_ptr<Node>&node
			)
		{
			return std::make_shared<Rule>(name,node);
		}
		
		struct Node
		{
			using FilterCallback=
			std::function<bool(const std::shared_ptr<SyntaxTree>&)>;
			
			enum class Tag:unsigned char
			{
				Word,
				Character,
				Any,
				Range,
				Sequence,
				Select,
				ZERO_OR_MORE,
				ONE_OR_MORE,
				Optional,
				Peek,
				Not,
				UpTo,
				Over,
				Repeat,
				Nil,
				Error,
				StrongRule,
				WeakRule,
				EoF,
				Filter
			}type;
			
			using Shared=std::shared_ptr<Node>;
			using String=std::basic_string<unsigned char>;
			using Boundary=std::pair<unsigned char,unsigned char>;
			using Batch=std::list<Shared>;
			using WeakRulePtr=std::weak_ptr<grammar::Rule>;
			using StrongRulePtr=std::shared_ptr<grammar::Rule>;
			using RepeatShared=std::pair<Shared,size_t>;
			
			explicit Node(char arg)
			:type(Tag::Character),aim{static_cast<unsigned char>(arg)}{}
			explicit Node(unsigned char arg)
			:type(Tag::Character),aim{arg}{}
			explicit Node(std::basic_string_view<char> str)
			:type(Tag::Word),word(reinterpret_cast<unsigned char const*>(str.begin()),str.length()){}
			explicit Node(std::basic_string_view<unsigned char>str)
			:type(Tag::Word),word(str){}
			
			union
			{
				Batch batch;
				Shared sub;
				WeakRulePtr weak;
				StrongRulePtr rule;
				String word; 
				Boundary range;
				FilterCallback filter;
				RepeatShared repeation;
				unsigned char aim;
			};
			
			Node(Tag s)												:type(s){}
			Node(Tag s,const Batch& arg)							:type(s),batch(arg){}
			Node(Tag s,const Shared& arg)							:type(s),sub(arg){}
			Node(Tag s,const WeakRulePtr& arg)						:type(s),weak(arg){}
			Node(Tag s,const StrongRulePtr& arg)					:type(s),rule(arg){}
			Node(Tag s,std::basic_string_view<unsigned char> arg)	:type(s),word(arg){}
			Node(Tag s,std::basic_string_view<char> arg)			:type(s),word(reinterpret_cast<unsigned char const*>(arg.begin()),arg.length()){}
			Node(Tag s,const Boundary& arg)							:type(s),range(arg){}
			Node(Tag s,const FilterCallback& arg)					:type(s),filter(arg){}
			Node(Tag s,const RepeatShared& arg)						:type(s),repeation(arg){}
			Node(Tag s,char arg)									:type(s),aim(arg){}
		public:
			~Node();
			
			class NodeOperatorDelegate
			{
				Shared data;
			public:
				operator Shared&() noexcept
				{
					return data;
				}
				
				NodeOperatorDelegate(char aim)
				:data{Node::Character(aim)}{}
				
				template<typename ...Args>
				NodeOperatorDelegate(Args&&...args) requires requires(){std::basic_string_view<char>(std::forward<Args>(args)...);}
				:data{Node::Word(std::basic_string_view<char>(std::forward<Args>(args)...))}{}
				
				template<typename ...Args>
				NodeOperatorDelegate(Args&&...args) requires requires(){Node::Shared(std::forward<Args>(args)...);}
				:data(std::forward<Args>(args)...){}
				
				NodeOperatorDelegate operator+()const noexcept
				{
					return Node::OneOrMore(data);
				}
				NodeOperatorDelegate operator*()const noexcept
				{
					return Node::ZeroOrMore(data);
				}
				NodeOperatorDelegate operator|(const NodeOperatorDelegate& that)const noexcept
				{
					if(data->type==Node::Tag::Select)
					{
						auto ret(data);
						ret->batch.push_back(that.data);
						return ret;
					}
					else if(that.data->type==Node::Tag::Select)
					{
						auto ret(that.data);
						ret->batch.push_front(data);
						return that;
					}
					else
						return Node::Select({data,that.data});
				}
				NodeOperatorDelegate operator%(const NodeOperatorDelegate& that)const noexcept
				{
					if(data->type==Node::Tag::Sequence)
					{
						auto ret(data);
						ret->batch.push_back(that.data);
						return ret;
					}
					else if(data->type==Node::Tag::Sequence)
					{
						auto ret(that.data);
						ret->batch.push_front(data);
						return that;
					}
					else
						return Node::Sequence({data,that.data});
				}
				NodeOperatorDelegate operator-()const noexcept
				{
					return Node::Not(data);
				}
				NodeOperatorDelegate operator&()const noexcept
				{
					return Node::Peek(data);
				}
				NodeOperatorDelegate operator-(const NodeOperatorDelegate& that) const noexcept
				{
					return (*this)% -that;
				}
				NodeOperatorDelegate operator&(const NodeOperatorDelegate& that)const noexcept
				{
					return (*this)% &that;
				}
				NodeOperatorDelegate operator!()const noexcept
				{
					return Node::Optional(data);
				}
				NodeOperatorDelegate operator*(size_t times)const noexcept
				{
					return Node::Repeat(data,times);
				}
			}; //operator delegate
			
			static Shared Character(char aim)
			{
				return std::make_shared<Node>(Tag::Character,aim);
			}
			static Shared Word(std::basic_string_view<char> word)
			{
				return std::make_shared<Node>(Tag::Word,word);
			}
			static Shared Character(unsigned char aim)
			{
				return std::make_shared<Node>(Tag::Character,aim);
			}
			static Shared Word(std::basic_string_view<unsigned char> word)
			{
				return std::make_shared<Node>(Tag::Word,word);
			}
			static Shared Any()
			{
				static const auto any(std::make_shared<Node>(Tag::Any));
				return any;
			}
			static Shared Range(char a, char b)
			{
				return std::make_shared<Node>(Tag::Range,Boundary{a,b});
			}
			static Shared Range(unsigned char a,unsigned char b)
			{
				return std::make_shared<Node>(Tag::Range,Boundary{a,b});
			}
			static Shared Sequence(const Batch &args)
			{
				return std::make_shared<Node>(Tag::Sequence,args);
			}
			static Shared Select(const Batch &args)
			{
				return std::make_shared<Node>(Tag::Select,args);
			}
			static Shared ZeroOrMore(const Shared &arg)
			{
				return std::make_shared<Node>(Tag::ZERO_OR_MORE,arg);
			}
			static Shared OneOrMore(const Shared &arg)
			{
				return std::make_shared<Node>(Tag::ONE_OR_MORE,arg);
			}
			static Shared Optional(const Shared &arg)
			{
				return std::make_shared<Node>(Tag::Optional,arg);
			}
			static Shared Peek(const Shared &arg)
			{
				return std::make_shared<Node>(Tag::Peek,arg);
			}
			static Shared Not(const Shared &arg)
			{
				return std::make_shared<Node>(Tag::Not,arg);
			}
			static Shared UpTo(const Shared &arg,size_t times)
			{
				return std::make_shared<Node>(Tag::UpTo,RepeatShared{arg,times});
			}
			static Shared Over(const Shared &arg,size_t times)
			{
				return std::make_shared<Node>(Tag::Over,RepeatShared{arg,times});
			}
			static Shared Repeat(const Shared &arg,size_t times)
			{
				return std::make_shared<Node>(Tag::Repeat,RepeatShared{arg,times});
			}
			static Shared Empty()
			{
				static const auto nil(std::make_shared<Node>(Tag::Nil));
				return nil;
			}
			static Shared Error()
			{
				static const auto error(std::make_shared<Node>(Tag::Error));
				return error;
			}
			static Shared Rule(const StrongRulePtr &rule)
			{
				return std::make_shared<Node>(Tag::StrongRule,rule);
			}
			static Shared WeakRule(const WeakRulePtr &rule)
			{
				return std::make_shared<Node>(Tag::WeakRule,rule);
			}
			static Shared EndOfFile()
			{
				static const auto eof(std::make_shared<Node>(Tag::EoF));
				return eof;
			}
			static Shared Filter(const FilterCallback &callback)
			{
				return std::make_shared<Node>(Tag::Filter,callback);
			}
		}; //class Node
		
		template<typename Letter>
		std::basic_ostream<Letter> &
		operator<<(std::basic_ostream<Letter> &stream,const Node &node)
		{
			using Tag=Node::Tag;
			
			switch (node.type)
			{
			case Tag::Character:
				stream<<"'"<<Letter(node.aim)<<"'";
				break;
			case Tag::Word:
				stream<<"'"<<std::basic_string_view<Letter>(reinterpret_cast<Letter const*>(node.word.c_str()),node.word.length())<<"'";
				break;
				
			case Tag::Any:
				stream<<'.';
				break;
				
			case Tag::Range:
				stream<<'['<<node.range.first<<'-'<<node.range.second<<']';
				break;
				
			case Tag::Sequence:
				stream<<"(";
				for (auto i:node.batch)
					stream<<i<<' ';
				stream<<")";
				break;
				
			case Tag::Select:
				stream<<"(";
				for (auto i:node.batch)
					stream<<*i<<'|';
				stream<<"<Error>";
				stream<<")";
				break;
				
			case Tag::ZERO_OR_MORE:
				stream<<*node.sub<<"*";
				break;
				
			case Tag::ONE_OR_MORE:
				stream<<*node.sub<<"+";
				break;
				
			case Tag::Optional:
				stream<<*node.sub<< "?";
				break;
				
			case Tag::Peek:
				stream<<"&"<<node.sub;
				break;
				
			case Tag::Not:
				stream<<"!"<<node.sub;
				break;
				
			case Tag::Over:
				stream<<node.repeation.first<<'{'<<node.repeation.second<<",}";
				break;
				
			case Tag::UpTo:
				stream<<node.repeation.first<<"{0,"<<node.repeation.second<<'}';
				break;
				
			case Tag::Repeat:
				stream<<node.repeation.first<<'{'<<node.repeation.second<<'}';
				break;
				
			case Tag::Nil:
				stream<<"''";
				break;
				
			case Tag::Error:
				stream<<"[]";
				break;
				
			case Tag::StrongRule:
				stream<<node.rule->name.c_str();
				break;
				
			case Tag::WeakRule:
				if (auto rule=node.weak.lock())
					stream<<rule->name.c_str();
				else
					stream<<"<DeletedRule>";
				break;
				
			case Tag::EoF:
				stream<<"<EoF>";
				break;
				
			case Tag::Filter:
				stream<<"<Filter>";
				break;
			}
			
			return stream;
		}
	}  // namespace grammar
}  // namespace peg_parser
