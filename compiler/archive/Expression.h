//
// Created by Louis Li on 2022/7/22.
//

#ifndef COMPILER_EXPRESSION_H
#define COMPILER_EXPRESSION_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APFixedPoint.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"

#include <memory>
#include <vector>

namespace reverSilly
{
	namespace Lang
	{
		using namespace llvm;
		using namespace std;
		namespace SyntaxTree {
			class Expression {
			public:
				virtual Value *codegen(bool is_static) const = 0;

				virtual bool is_constant() const = 0;

				virtual bool is_static() const = 0;

				virtual ~Expression() = 0;
				//TODO: implement these classes
			};

			class Object : Expression
			{
			public:
				unique_ptr<Expression> content;

				virtual Value *codegen(bool is_static) const override
				{
					return content->codegen(is_static);
				}

				~Object() override = default;
			};

			class If : Expression {
			public:
				unique_ptr<Expression> condition;
				unique_ptr<Expression> ifTrue, ifFalse;

				Value *codegen(bool is_static) const override {
					//TODO: implement this
					return 0;
				}

				~If() = default;
			};

			class Call : Expression {
				unique_ptr<Expression> callee, argument;

				Value *codegen(bool is_static) const override {
					//TODO:
					return 0;
				}
			};

			class Prototype : Expression {
				bool is_constant() const override {
					return true;
				}
			};

			class New : Expression {

			};

			class Allocate : Expression
			{
				bool is_constant() const override
				{
					return true;
				}

			};

			class Shape : Expression {

			};

			class Null : Expression {
				bool is_constant() const override
				{
					return true;
				}
				bool is_static() const override
				{
					return true;
				}
				Value * codegen() const override
				{
					return llvm::ConstantInt::getValueID();
				}
			};
		}
	} // reverSilly
} // Lang

#endif //COMPILER_EXPRESSION_H
