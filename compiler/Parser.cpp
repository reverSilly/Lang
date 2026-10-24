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
				(const unordered_map<string_view, typename Interpreter<unique_ptr<Expression>, Object *>::Callback> &callbacks = {}) {
				using namespace grammar;
				ParserGenerator<unique_ptr<Expression>, Object *> psr;
				auto SetRule
					{
						[&psr, &callbacks](string_view name, const Node::Shared &grammar) {
							if (auto item = callbacks.find(name);item != callbacks.end())
								return Node::Rule(psr.setRule(name, grammar, item->second));
							else
								return Node::Rule(psr.setRule(name, grammar));
						}
					};
				auto nonNull{Node::Filter([](const auto &tree) -> bool {
//                cerr<<':'<<*tree<<'\n'<<'@'<<tree->cview()<<'\n';
					auto *i = tree.get();
					for (; !i->inner.empty(); i = i->inner.back().get());
					return i->rule->name != "Null";
				})};
				auto ExprFwd{psr.getRule("Expression")};
				auto Escaped{SetRule("Escaped", Node::Sequence({Node::Character('\\'), Node::Any()}))};
				SetRule
					(
						"WhiteSpace",
						Node::Select
							({
								 Node::Character('\r'),
								 Node::Character('\n'),
								 Node::Character('\t'),
								 Node::Character('\0'),
								 Node::Character('\x0b'),
								 Node::Character('\x0c'),
								 Node::Character(' '),
							 })
					);
				SetRule
					(
						"Hint",
						Node::Sequence
							({
								 Node::Character('!'),
								 Node::ZeroOrMore(Node::Select
													  ({
														   Node::Range('\x00', '!' - 1),
														   Node::Range('!' + 1, '\xff'),
														   Escaped,
													   })),
								 Node::Character('!')
							 })
					);
				auto Separator{
					psr.setRule("Separator", Node::Select({psr.getRuleNode("WhiteSpace"), psr.getRuleNode("Hint")}))};
				auto Air{Node::ZeroOrMore(Node::Rule(Separator))};
				auto Infix
					{
						[&psr, &Air](std::string_view next, const Node::Shared &separator) {
							auto Next{Node::Rule(psr.getRule(next))};
							return Node::Sequence
								({
									 Next,
									 Node::ZeroOrMore(Node::Sequence
														  ({
															   separator,
															   Air,
															   Next
														   }))
								 });
						}
					};
				auto Prefix
					{
						[&SetRule, &psr, &Air](string_view next,
											   unordered_map<string_view, const Node::Shared &> &&prefixes) {
							auto Next{Node::Rule(psr.getRule(next))};
							auto prefix{Node::Select({})};
							for (auto &[i, j]: prefixes)
								prefix->batch.push_back(SetRule(i, Node::OneOrMore(Node::Sequence({j, Air}))));
							return Node::Sequence({Node::ZeroOrMore(prefix), Next});
						}
					};
				auto Suffix
					{
						[&SetRule, &Air, &psr, &nonNull](std::string_view next,
														 unordered_map<string_view, const Node::Shared &> &&suffixes) {
							auto Next{Node::Rule(psr.getRule(next))};
							auto suffix{Node::Select({})};
							auto nonext{Node::Not(Node::Sequence({Next, nonNull}))};
							for (auto &[i, j]: suffixes) {

								suffix->batch.push_back(SetRule(i, Node::OneOrMore(Node::Sequence({j, Air, nonext}))));
							}
							return Node::Sequence({Next, Node::ZeroOrMore(suffix)});
						}
					};
				SetRule
					(
						"Char",
						Node::Select
							({
								 Escaped,
								 Node::Range(static_cast<unsigned char>(1), static_cast<unsigned char>(8)),
								 Node::Range(static_cast<unsigned char>(14), static_cast<unsigned char>(31)),
								 Node::Range('0', '9'),
								 Node::Range('A', 'Z'),
								 Node::Range('a', 'z'),
								 Node::Range(static_cast<unsigned char>(127), static_cast<unsigned char>(255)),
								 Node::Character('_')
							 })
					);
				auto Chars
					{
						SetRule
							(
								"Chars",
								Node::Sequence
									({
										 Node::Not(Node::Word("if")),
										 Node::Not(Node::Word("else")),
										 Node::Not(Node::Word("match")),
										 Node::Not(Node::Word("with")),
										 Node::Not(Node::Word("shape")),
										 Node::Not(Node::Word("new")),
										 Node::Not(Node::Word("allocate")),
										 Node::OneOrMore(Node::Rule(psr.getRule("Char"))),
										 Air
									 })
							)
					};
				auto BitsLiteral
					{
						SetRule
							(
								"BitsLiteral",
								Node::Sequence
									({
										 Node::Character('\"'),
										 Node::ZeroOrMore(Node::Select
															  ({
																   Node::Range('\x00', '\"' - 1),
																   Node::Range('\"' + 1, '\xff'),
																   Escaped
															   })),
										 Node::Character('\"'),
										 Air
									 })
							)
					};
				auto Object{SetRule("Object", Node::Sequence(
					{Node::Character('{'), Node::WeakRule(ExprFwd), Node::Character('}'), Air}))};
				auto Bracket{SetRule("Bracket", Node::Sequence(
					{Node::Character('('), Node::WeakRule(ExprFwd), Node::Character(')'), Air}))};
				auto Null{SetRule("Null", Air)};
				SetRule
					(
						"Unit",
						Node::Select
							({
								 BitsLiteral,
								 Object,
								 Bracket,
								 Chars,
								 Null
							 })
					);
				SetRule("Access", Infix("Unit", Node::Character('.')));//2
				SetRule("Call", Infix("Access", Node::Character('@')));
				SetRule
					(
						"Suffixes",
						Suffix
							(
								"Call",
								{
									{"Pioneer",   Node::Character('<')},
									{"Successor", Node::Character('>')},
									{"RightType", Node::Character('$')},
									{"LeftType",  Node::Character('`')}
								}
							)
					);
				SetRule
					(
						"Prefixes",
						Prefix
							(
								"Suffixes",
								{
									{"Prototype",  Node::Character('^')},
									{"Shape",      Node::Word("shape")},
									{"New",        Node::Word("new")},
									{"Allocate",   Node::Word("allocate")},
									{"Not",        Node::Character('~')},
									{"Positive",   Node::Character('+')},
									{"Negative",   Node::Character('-')},
									{"Star",       Node::Character('*')},
									{"Reverse",    Node::Character('/')},
									{"RightValue", Node::Character('$')},
									{"LeftValue",  Node::Character('`')}
								}
							)
					);
				SetRule("Modular", Infix("Prefixes", Node::Character('%')));//18
				SetRule("Divide", Infix("Modular", Node::Character('/')));//19
				SetRule("Multiply", Infix("Divide", Node::Character('*')));
				SetRule("Minus", Infix("Multiply", Node::Character('-')));//21
				SetRule("Add", Infix("Minus", Node::Character('+')));//22
				SetRule("LeftShift", Infix("Add", Node::Word("#<")));//23
				SetRule("RightShift", Infix("LeftShift", Node::Word("#>")));
				SetRule("Less", Infix("RightShift", Node::Character('<')));
				SetRule("Greater", Infix("Less", Node::Character('>')));
				SetRule("NotLess", Infix("Greater", Node::Word("~<")));
				SetRule("NotGreater", Infix("NotLess", Node::Word("~>")));
				SetRule("Equal", Infix("NotGreater", Node::Word("==")));
				SetRule("NotEqual", Infix("Equal", Node::Word("~=")));
				SetRule("Compare", Infix("NotEqual", Node::Word("<=>")));
				SetRule("And", Infix("Compare", Node::Character('&')));
				SetRule("Xor", Infix("And", Node::Character('~')));
				SetRule("Or", Infix("Xor", Node::Character('|')));
				SetRule("Concatenate", Infix("Or", Node::Character('#')));
				SetRule("Map", Infix("Concatenate", Node::Word("->")));
				auto Require{SetRule("Require", Infix("Map", Node::Character(':')))};
				auto If
					{
						SetRule
							(
								"If",
								Node::Sequence
									({
										 Require,
										 Node::Optional(Node::Sequence
															({
																 Node::Word("if"),
																 Air,
																 Require,
																 Node::Optional(
																	 Node::Sequence({Node::Word("else"), Require}))
															 }))
									 })
							)
					};
				SetRule
					(
						"Match",
						Node::Select
							({
								 Node::Sequence
									 ({
										  Node::Word("match"),
										  Air,
										  If,
										  Node::OneOrMore(Node::Sequence
															  ({
																   Node::Word("with"),
																   Air,
																   If
															   }))
									  }),
								 If,
							 })
					);
				SetRule("Assign", Infix("Match", Node::Character('=')));
				SetRule("rDefine", Infix("Assign", Node::Word("=:")));
				SetRule("Define", Infix("rDefine", Node::Word(":=")));
				SetRule("Comma", Infix("Define", Node::Character(',')));
				auto Semicolon{SetRule("Semicolon", Infix("Comma", Node::Character(';')))};
				SetRule
					(
						"Expression",
						Node::Sequence({Air, Semicolon, Node::ZeroOrMore({Node::Sequence({Semicolon, nonNull})})})
					);
				psr.setStart(ExprFwd);
				return psr;
			}
			unordered_map<string_view,Interpreter<unique_ptr<Expression>,Object*>::Callback>DefaultCallbacks
			{
				{"Null"sv,[](auto &e,Object*){return make_unique<Null>();}},

			};
		}
	}
}
//Suffixes(Call(Access(Unit(Chars(Char('1'),),),),),Pioneer('<'),Call(Access(Unit(Null(''),),),),)