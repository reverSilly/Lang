#pragma once

#include "interpreter.h"

namespace peg_parser
{
	namespace Presets
	{		
		Program<int> createIntegerProgram();
		Program<float> createFloatProgram();
		Program<double> createDoubleProgram();
		Program<int> createHexProgram();
		std::function<char(std::basic_string_view<char>)> defaultEscapeCodeCallback();
		Program<char> createCharacterProgram
		(
			const std::function<char(std::basic_string_view<char>)> escapeCodeCallback=defaultEscapeCodeCallback(),
			const grammar::Node::Shared& escapeSequence=
			grammar::Node::Any()
			);
		Program<std::basic_string<char>>createStringProgram
		(
			std::basic_string_view<char> open,
			std::basic_string_view<char> close
			);
		
		using RuleGetter=const std::function<typename grammar::Node::Shared(std::basic_string_view<char>)>&;
		using GrammarProgram=Program<typename grammar::Node::Shared,RuleGetter&>;
		GrammarProgram createParserParser();
	};  // namespace presets
}  // namespace peg_parser
