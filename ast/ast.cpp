#include "..\main.h"

static constexpr uint64_t DOUBLE_SIGN = 0x8000000000000000;
static constexpr uint64_t DOUBLE_EXPONENT = 0x7FF0000000000000;
static constexpr uint64_t DOUBLE_FRACTION = 0x000FFFFFFFFFFFFF;
static constexpr uint64_t DOUBLE_SPECIAL = DOUBLE_EXPONENT;
static constexpr uint64_t DOUBLE_NEGATIVE_ZERO = DOUBLE_SIGN;

Ast::Ast(const Bytecode& bytecode) : bytecode(bytecode) {}

Ast::~Ast() {
	for (uint32_t i = statements.size(); i--;) {
		delete statements[i];
	}

	for (uint32_t i = functions.size(); i--;) {
		delete functions[i];
	}

	for (uint32_t i = expressions.size(); i--;) {
		delete expressions[i];
	}
}

Ast::Function*& Ast::new_function(const Bytecode::Prototype& prototype, const uint32_t& level) {
	functions.emplace_back(new Function(prototype, level));
	return functions.back();
}

Ast::Statement*& Ast::new_statement(const AST_STATEMENT& type) {
	statements.emplace_back(new Statement(type));
	return statements.back();
}

Ast::Expression*& Ast::new_expression(const AST_EXPRESSION& type) {
	expressions.emplace_back(new Expression(type));
	return expressions.back();
}

void Ast::operator()() {
	chunk = new_function(*bytecode.main);
	if (bytecode.header.version == Bytecode::BC_VERSION_2) isFR2Enabled = bytecode.header.flags & Bytecode::BC_F_FR2;
	prototypeDataLeft = bytecode.prototypesTotalSize;
	print_progress_bar();
	build_functions(*chunk);
	erase_progress_bar();
	functions.shrink_to_fit();
	statements.shrink_to_fit();
	expressions.shrink_to_fit();
}

void Ast::build_functions(Function& function) {
	build_instructions(function);
	function.usedGlobals.shrink_to_fit();
	if (!function.hasDebugInfo) function.slotScopeCollector.build_upvalue_scopes();
	collect_slot_scopes(function, function.block);
	assert(function.slotScopeCollector.assert_scopes_closed(), "Failed to close slot scopes", bytecode.filePath, DEBUG_INFO);
	eliminate_slots(function, function.block);
	eliminate_conditions(function, function.block);
	build_if_statements(function, function.block);
	function.block.shrink_to_fit();
	prototypeDataLeft -= function.prototype.prototypeSize;
	print_progress_bar(bytecode.prototypesTotalSize - prototypeDataLeft, bytecode.prototypesTotalSize);

	for (uint32_t i = function.childFunctions.size(); i--;) {
		function.childFunctions[i]->id = nextFunctionId;
		nextFunctionId++;
		build_functions(*function.childFunctions[i]);
	}
}

void Ast::build_instructions(Function& function) {
	std::vector<uint8_t> upvalues;
	function.block.resize(function.prototype.instructions.size(), nullptr);

	for (uint32_t i = function.block.size(); i--;) {
		function.block[i] = new_statement(AST_STATEMENT_INSTRUCTION);
		function.block[i]->instruction.type = function.prototype.instructions[i].type;
		function.block[i]->instruction.a = function.prototype.instructions[i].a;
		function.block[i]->instruction.b = function.prototype.instructions[i].b;
		function.block[i]->instruction.c = function.prototype.instructions[i].c;
		function.block[i]->instruction.d = function.prototype.instructions[i].d;
		function.block[i]->instruction.id = i;

		switch (function.block[i]->instruction.type) {
		case Bytecode::BC_OP_FNEW:
			function.block[i]->function = new_function(*function.get_constant(function.block[i]->instruction.d).prototype);
			function.childFunctions.emplace_back(function.block[i]->function);
			function.block[i]->function->upvalues.resize(function.block[i]->function->prototype.upvalues.size());

			for (uint8_t j = function.block[i]->function->upvalues.size(); j--;) {
				function.block[i]->function->upvalues[j].slot = function.block[i]->function->prototype.upvalues[j];

				if (!(function.block[i]->function->prototype.upvalues[j] & Bytecode::BC_UV_LOCAL)) {
					function.block[i]->function->upvalues[j].slotScope = function.upvalues[function.block[i]->function->upvalues[j].slot].slotScope;
					continue;
				}

				function.block[i]->function->upvalues[j].local = true;
				if (function.block[i]->function->upvalues[j].slot >= function.prototype.header.parameters) upvalues.emplace_back(function.block[i]->function->upvalues[j].slot);
			}

			if (upvalues.size()) {
				function.slotScopeCollector.add_upvalues(function.block[i]->instruction.id, upvalues);
				upvalues.clear();
			}

			continue;
		case Bytecode::BC_OP_CALLMT:
		case Bytecode::BC_OP_CALLT:
		case Bytecode::BC_OP_RETM:
		case Bytecode::BC_OP_RET:
		case Bytecode::BC_OP_RET0:
		case Bytecode::BC_OP_RET1:
			function.block[i]->type = AST_STATEMENT_RETURN;
			continue;
		case Bytecode::BC_OP_UCLO:
		case Bytecode::BC_OP_ISNEXT:
		case Bytecode::BC_OP_FORI:
		case Bytecode::BC_OP_FORL:
		case Bytecode::BC_OP_ITERL:
		case Bytecode::BC_OP_LOOP:
		case Bytecode::BC_OP_JMP:
			function.block[i]->instruction.target = function.block[i]->instruction.id + (function.block[i]->instruction.d - Bytecode::BC_OP_JMP_BIAS + 1);
			continue;
		}
	}

	function.childFunctions.shrink_to_fit();
	return assign_debug_info(function);
}

void Ast::assign_debug_info(Function& function) {
	if (!function.hasDebugInfo) return group_jumps(function);
	std::vector<uint32_t> activeLocalScopes;
	function.parameterNames.resize(function.prototype.header.parameters);

	for (uint8_t i = function.parameterNames.size(); i--;) {
		function.parameterNames[i] = function.prototype.variableInfos[i].name;
		activeLocalScopes.emplace_back(function.prototype.variableInfos[i].scopeEnd);
	}

	uint32_t index;

	for (uint32_t i = function.parameterNames.size(); i < function.prototype.variableInfos.size(); i++) {
		assert(!activeLocalScopes.size()
			|| function.prototype.variableInfos[i].scopeBegin > activeLocalScopes.back()
			|| function.prototype.variableInfos[i].scopeEnd <= activeLocalScopes.back()
			|| function.prototype.variableInfos[i].scopeBegin == activeLocalScopes.back(),
			"Illegal variable scope border overlap", bytecode.filePath, DEBUG_INFO);

		while (activeLocalScopes.size() && function.prototype.variableInfos[i].scopeEnd > activeLocalScopes.back()) {
			activeLocalScopes.pop_back();
		}

		if (function.prototype.variableInfos[i].type != Bytecode::BC_VAR_STR) {
			activeLocalScopes.emplace_back(function.prototype.variableInfos[i].scopeEnd);
			continue;
		}

		if (function.locals.size()
			&& function.prototype.variableInfos[i].scopeBegin == function.prototype.variableInfos[i].scopeEnd
			&& function.locals.back().scopeEnd == function.prototype.variableInfos[i].scopeEnd) {
			index = get_block_index_from_id(function.block, function.prototype.variableInfos[i].scopeBegin);

			switch (function.block[index]->instruction.type) {
			case Bytecode::BC_OP_KPRI:
				if (function.block[index]->instruction.d) break;
			case Bytecode::BC_OP_KNIL:
				if ((function.block[index]->instruction.type == Bytecode::BC_OP_KPRI ? function.block[index]->instruction.a : function.block[index]->instruction.d) < activeLocalScopes.size()) {
					while (activeLocalScopes.size() != function.locals.back().baseSlot) {
						assert(activeLocalScopes.size() && activeLocalScopes.back() == function.prototype.variableInfos[i].scopeEnd, "Unable to build variable scope", bytecode.filePath, DEBUG_INFO);
						activeLocalScopes.pop_back();
					}

					function.locals.emplace_back();
					function.locals.back().baseSlot = activeLocalScopes.size();
					function.locals.back().scopeBegin = function.prototype.variableInfos[i].scopeBegin;
					function.locals.back().scopeEnd = function.prototype.variableInfos[i].scopeEnd;
					function.locals.back().excludeBlock = function.locals[function.locals.size() - 2].scopeBegin
						== function.locals[function.locals.size() - 2].scopeEnd ? function.locals[function.locals.size() - 2].excludeBlock : true;
				}
			}
		}

		if (!function.locals.size()
			|| function.prototype.variableInfos[i].scopeBegin != function.locals.back().scopeBegin
			|| function.prototype.variableInfos[i].scopeEnd != function.locals.back().scopeEnd) {
			function.locals.emplace_back();
			function.locals.back().baseSlot = activeLocalScopes.size();
			function.locals.back().scopeBegin = function.prototype.variableInfos[i].scopeBegin;
			function.locals.back().scopeEnd = function.prototype.variableInfos[i].scopeEnd;
		}

		function.locals.back().names.emplace_back(function.prototype.variableInfos[i].name);
		activeLocalScopes.emplace_back(function.locals.back().scopeEnd);
	}

	for (uint32_t i = function.locals.size(); i--;) {
		function.locals[i].names.shrink_to_fit();
	}

	function.locals.shrink_to_fit();
	return group_jumps(function);
}

void Ast::group_jumps(Function& function) {
	for (uint32_t i = function.block.size(); i--;) {
		switch (function.block[i]->instruction.type) {
		case Bytecode::BC_OP_ISTC:
		case Bytecode::BC_OP_ISFC:
			function.add_jump(function.block[i]->instruction.id, function.block[i]->instruction.id + 2);
		case Bytecode::BC_OP_ISLT:
		case Bytecode::BC_OP_ISGE:
		case Bytecode::BC_OP_ISLE:
		case Bytecode::BC_OP_ISGT:
		case Bytecode::BC_OP_ISEQV:
		case Bytecode::BC_OP_ISNEV:
		case Bytecode::BC_OP_ISEQS:
		case Bytecode::BC_OP_ISNES:
		case Bytecode::BC_OP_ISEQN:
		case Bytecode::BC_OP_ISNEN:
		case Bytecode::BC_OP_ISEQP:
		case Bytecode::BC_OP_ISNEP:
		case Bytecode::BC_OP_IST:
		case Bytecode::BC_OP_ISF:
			function.block[i]->type = AST_STATEMENT_CONDITION;
			function.block[i]->instruction.target = function.block[i + 1]->instruction.target;
			function.block.erase(function.block.begin() + i + 1);
			function.slotScopeCollector.add_jump(function.block[i]->instruction.id + 1, function.block[i]->instruction.target);
			continue;
		case Bytecode::BC_OP_UCLO:
			function.slotScopeCollector.add_upvalue_close(function.block[i]->instruction.id, function.block[i]->instruction.target, function.block[i]->instruction.a);
		case Bytecode::BC_OP_JMP:
			function.block[i]->type = AST_STATEMENT_GOTO;
		case Bytecode::BC_OP_LOOP:
			function.add_jump(function.block[i]->instruction.id, function.block[i]->instruction.target);
			continue;
		}
	}

	function.labels.shrink_to_fit();
	uint32_t index;

	for (uint32_t i = function.block.size(); i--;) {
		function.block[i]->instruction.attachedLabel = function.get_label_from_id(function.block[i]->instruction.id);

		switch (function.block[i]->instruction.type) {
		case Bytecode::BC_OP_UCLO:
			if (function.block[i]->instruction.target == get_extended_id_from_statement(function.block[i + 1])) {
				function.block[i]->type = AST_STATEMENT_EMPTY;
				function.remove_jump(function.block[i]->instruction.id, function.block[i]->instruction.target);
			}

			continue;
		case Bytecode::BC_OP_ITERC:
			index = get_block_index_from_id(function.block, function.labels[function.block[i]->instruction.attachedLabel].jumpIds.front());
			function.block[index]->type = AST_STATEMENT_INSTRUCTION;
			function.remove_jump(function.block[index]->instruction.id, function.block[index]->instruction.target);
			continue;
		case Bytecode::BC_OP_JMP:
			if (function.block[i]->type != AST_STATEMENT_GOTO) continue;
			function.slotScopeCollector.add_jump(function.block[i]->instruction.id, function.block[i]->instruction.target);
			if (function.block[i]->instruction.target == function.block[i]->instruction.id
				|| !i
				|| function.block[i - 1]->instruction.type != Bytecode::BC_OP_JMP
				|| function.block[i - 1]->instruction.d != Bytecode::BC_OP_JMP_BIAS)
				continue;
			function.remove_jump(function.block[i - 1]->instruction.id, function.block[i - 1]->instruction.id);
			function.block[i - 1]->type = AST_STATEMENT_CONDITION;
			function.block[i - 1]->instruction.target = function.block[i]->instruction.target;
			function.block.erase(function.block.begin() + i);
			continue;
		}
	}

	for (uint32_t i = function.block.size(); i--;) {
		if (i
			&& function.block[i]->type == AST_STATEMENT_RETURN
			&& function.block[i - 1]->type == AST_STATEMENT_RETURN
			&& function.is_valid_label(function.block[i]->instruction.attachedLabel)
			&& function.labels[function.block[i]->instruction.attachedLabel].jumpIds.size() == 1) {
			index = get_block_index_from_id(function.block, function.labels[function.block[i]->instruction.attachedLabel].jumpIds.back());

			if (index != INVALID_ID
				&& function.block[index]->instruction.type == Bytecode::BC_OP_UCLO) {
				function.remove_jump(function.block[index]->instruction.id, function.block[index]->instruction.target);
				function.block[index]->instruction.type = function.block[i]->instruction.type;
				function.block[index]->instruction.a = function.block[i]->instruction.a;
				function.block[index]->instruction.b = function.block[i]->instruction.b;
				function.block[index]->instruction.c = function.block[i]->instruction.c;
				function.block[index]->instruction.d = function.block[i]->instruction.d;
				function.block[i]->type = AST_STATEMENT_EMPTY;
				continue;
			}
		}

		if (function.block[i]->instruction.type == Bytecode::BC_OP_RET0) function.block[i]->type = AST_STATEMENT_EMPTY;
		break;
	}

	return build_loops(function);
}

void Ast::build_loops(Function& function) {
	static void (* const build_break_statements)(std::vector<Statement*>& block, const uint32_t& breakTarget) = [](std::vector<Statement*>& block, const uint32_t& breakTarget)->void {
		for (uint32_t i = block.size(); i--;) {
			if (block[i]->type != AST_STATEMENT_GOTO || block[i]->instruction.target != breakTarget) continue;
			block[i]->type = AST_STATEMENT_BREAK;
		}
	};

	uint32_t targetIndex, breakTarget;

	for (uint32_t i = function.block.size(); i--;) {
		if (function.block[i]->type != AST_STATEMENT_INSTRUCTION) continue;

		switch (function.block[i]->instruction.type) {
		case Bytecode::BC_OP_ISNEXT:
		case Bytecode::BC_OP_JMP:
			function.block[i]->type = AST_STATEMENT_GENERIC_FOR;
			targetIndex = get_block_index_from_id(function.block, function.block[i]->instruction.target);
			breakTarget = get_extended_id_from_statement(function.block[targetIndex + 2]);
			function.block[i]->instruction = function.block[targetIndex]->instruction;
			function.block[i]->instruction.id = function.block[targetIndex + 1]->instruction.target - 1;
			function.block[i]->instruction.target = function.block[targetIndex + 1]->instruction.id + 1;
			function.block[targetIndex]->type = AST_STATEMENT_EMPTY;
			function.block[i]->block.reserve(targetIndex - i);
			function.block[i]->block.insert(function.block[i]->block.begin(), function.block.begin() + i + 1, function.block.begin() + targetIndex + 1);
			function.block.erase(function.block.begin() + i + 1, function.block.begin() + targetIndex + 2);
			function.slotScopeCollector.add_loop(function.block[i]->instruction.id, function.block[i]->instruction.target);
			build_break_statements(function.block[i]->block, breakTarget);
			build_local_scopes(function, function.block[i]->block);
			continue;
		case Bytecode::BC_OP_FORI:
			function.block[i]->type = AST_STATEMENT_NUMERIC_FOR;
			targetIndex = get_block_index_from_id(function.block, function.block[i]->instruction.target);
			breakTarget = get_extended_id_from_statement(function.block[targetIndex]);
			function.block[targetIndex - 1]->type = AST_STATEMENT_EMPTY;
			function.block[i]->block.reserve(targetIndex - 1 - i);
			function.block[i]->block.insert(function.block[i]->block.begin(), function.block.begin() + i + 1, function.block.begin() + targetIndex);
			function.block.erase(function.block.begin() + i + 1, function.block.begin() + targetIndex);
			function.slotScopeCollector.add_loop(function.block[i]->instruction.id, function.block[i]->instruction.target);
			build_break_statements(function.block[i]->block, breakTarget);
			build_local_scopes(function, function.block[i]->block);
			continue;
		case Bytecode::BC_OP_LOOP:
			assert(function.block[i]->instruction.target >= function.block[i]->instruction.id, "LOOP instruction has invalid jump target", bytecode.filePath, DEBUG_INFO);
			function.remove_jump(function.block[i]->instruction.id, function.block[i]->instruction.target);

			if (function.block[i]->instruction.target == function.block[i]->instruction.id) {
				assert(i + 1 < function.block.size()
					&& function.block[i + 1]->type == AST_STATEMENT_GOTO
					&& function.block[i + 1]->instruction.target <= function.block[i]->instruction.id
					&& !function.is_valid_label(function.block[i + 1]->instruction.attachedLabel),
					"Invalid goto loop", bytecode.filePath, DEBUG_INFO);
				function.block[i]->type = AST_STATEMENT_EMPTY;
				function.block[i + 1]->instruction.type = function.block[i]->instruction.type;
				continue;
			}

			function.block[i]->type = AST_STATEMENT_LOOP;
			targetIndex = get_block_index_from_id(function.block, function.block[i]->instruction.target);
			breakTarget = get_extended_id_from_statement(function.block[targetIndex]);
			function.block[i]->block.reserve(targetIndex - 1 - i);
			function.block[i]->block.insert(function.block[i]->block.begin(), function.block.begin() + i + 1, function.block.begin() + targetIndex);
			function.block.erase(function.block.begin() + i + 1, function.block.begin() + targetIndex);
			function.slotScopeCollector.add_loop(function.block[i]->instruction.id, function.block[i]->instruction.target);
			build_break_statements(function.block[i]->block, breakTarget);

			if (function.block[i]->block.size()
				&& function.block[i]->block.back()->type == AST_STATEMENT_CONDITION
				&& function.is_valid_label(function.block[i]->instruction.attachedLabel)
				&& breakTarget != function.block[i]->instruction.id) {
				for (uint32_t j = function.labels[function.block[i]->instruction.attachedLabel].jumpIds.size(); j--
					&& function.labels[function.block[i]->instruction.attachedLabel].jumpIds[j] > function.block[i]->instruction.id;) {
					if (function.labels[function.block[i]->instruction.attachedLabel].jumpIds[j] >= function.block[i]->instruction.target) continue;
					targetIndex = get_block_index_from_id(function.block[i]->block, function.labels[function.block[i]->instruction.attachedLabel].jumpIds[j] - 1);

					if (targetIndex != INVALID_ID && function.block[i]->block[targetIndex]->type == AST_STATEMENT_CONDITION) {
						//HACK
						function.block[i]->block.emplace_back(new_statement(AST_STATEMENT_BREAK));
						function.block[i]->block.back()->instruction.type = Bytecode::BC_OP_JMP;
						function.block[i]->block.back()->instruction.target = breakTarget;
						function.block[i]->block.emplace_back(new_statement(AST_STATEMENT_GOTO));
						function.block[i]->block.back()->instruction.type = Bytecode::BC_OP_JMP;
						function.block[i]->block.back()->instruction.target = function.block[i]->instruction.id;
					}

					break;
				}
			}

			build_local_scopes(function, function.block[i]->block);
			continue;
		}
	}

	function.slotScopeCollector.upvalueInfos.shrink_to_fit();
	return build_local_scopes(function, function.block);
}

void Ast::build_local_scopes(Function& function, std::vector<Statement*>& block) {
	if (!function.hasDebugInfo) return build_expressions(function, block);
	uint32_t scopeBeginIndex, scopeEndIndex;

	for (uint32_t i = function.locals.size(); i--;) {
		scopeBeginIndex = get_block_index_from_id(block, function.locals[i].scopeBegin);
		if (scopeBeginIndex == INVALID_ID) continue;

		switch (block[scopeBeginIndex]->type) {
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			block[scopeBeginIndex]->locals = &function.locals[i];
			continue;
		}

		scopeBeginIndex++;
		block.emplace(block.begin() + scopeBeginIndex, new_statement(AST_STATEMENT_DECLARATION));
		block[scopeBeginIndex]->locals = &function.locals[i];

		if (function.locals[i].scopeEnd > function.locals[i].scopeBegin) {
			block[scopeBeginIndex]->instruction.id = function.locals[i].scopeBegin + 1;
			scopeEndIndex = get_block_index_from_id(block, function.locals[i].scopeEnd + 1);
			if (scopeEndIndex == INVALID_ID) scopeEndIndex = block.size();

			while (block[scopeEndIndex - 1]->type == AST_STATEMENT_DECLARATION && block[scopeEndIndex - 1]->locals->excludeBlock) {
				scopeEndIndex--;
			}

			block[scopeBeginIndex]->block.reserve(scopeEndIndex - 1 - scopeBeginIndex);
			block[scopeBeginIndex]->block.insert(block[scopeBeginIndex]->block.begin(), block.begin() + scopeBeginIndex + 1, block.begin() + scopeEndIndex);
			block.erase(block.begin() + scopeBeginIndex + 1, block.begin() + scopeEndIndex);
			build_expressions(function, block[scopeBeginIndex]->block);
		}
	}

	return build_expressions(function, block);
}

void Ast::build_expressions(Function& function, std::vector<Statement*>& block) {
	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_INSTRUCTION:
			block[i]->type = AST_STATEMENT_ASSIGNMENT;
			block[i]->assignment.expressions.resize(1, nullptr);

			switch (block[i]->instruction.type) {
			case Bytecode::BC_OP_MOV:
				block[i]->assignment.expressions.back() = new_slot(block[i]->instruction.d);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
				break;
			case Bytecode::BC_OP_NOT:
			case Bytecode::BC_OP_UNM:
			case Bytecode::BC_OP_LEN:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_UNARY_OPERATION);

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_NOT:
					block[i]->assignment.expressions.back()->unaryOperation->type = AST_UNARY_NOT;
					block[i]->assignment.allowedConstantType = INVALID_CONSTANT;
					break;
				case Bytecode::BC_OP_UNM:
					block[i]->assignment.expressions.back()->unaryOperation->type = AST_UNARY_MINUS;
					block[i]->assignment.allowedConstantType = BOOL_CONSTANT;
					break;
				case Bytecode::BC_OP_LEN:
					block[i]->assignment.expressions.back()->unaryOperation->type = AST_UNARY_LENGTH;
					break;
				}

				block[i]->assignment.expressions.back()->unaryOperation->operand = new_slot(block[i]->instruction.d);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->unaryOperation->operand);
				break;
			case Bytecode::BC_OP_ADDVN:
			case Bytecode::BC_OP_SUBVN:
			case Bytecode::BC_OP_MULVN:
			case Bytecode::BC_OP_DIVVN:
			case Bytecode::BC_OP_MODVN:
			case Bytecode::BC_OP_ADDNV:
			case Bytecode::BC_OP_SUBNV:
			case Bytecode::BC_OP_MULNV:
			case Bytecode::BC_OP_DIVNV:
			case Bytecode::BC_OP_MODNV:
			case Bytecode::BC_OP_ADDVV:
			case Bytecode::BC_OP_SUBVV:
			case Bytecode::BC_OP_MULVV:
			case Bytecode::BC_OP_DIVVV:
			case Bytecode::BC_OP_MODVV:
			case Bytecode::BC_OP_POW:
				block[i]->assignment.allowedConstantType = BOOL_CONSTANT;
			case Bytecode::BC_OP_CAT:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_BINARY_OPERATION);

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_ADDVN:
				case Bytecode::BC_OP_ADDNV:
				case Bytecode::BC_OP_ADDVV:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_ADDITION;
					break;
				case Bytecode::BC_OP_SUBVN:
				case Bytecode::BC_OP_SUBNV:
				case Bytecode::BC_OP_SUBVV:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_SUBTRACTION;
					break;
				case Bytecode::BC_OP_MULVN:
				case Bytecode::BC_OP_MULNV:
				case Bytecode::BC_OP_MULVV:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_MULTIPLICATION;
					break;
				case Bytecode::BC_OP_DIVVN:
				case Bytecode::BC_OP_DIVNV:
				case Bytecode::BC_OP_DIVVV:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_DIVISION;
					break;
				case Bytecode::BC_OP_MODVN:
				case Bytecode::BC_OP_MODNV:
				case Bytecode::BC_OP_MODVV:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_MODULO;
					break;
				case Bytecode::BC_OP_POW:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_EXPONENTATION;
					break;
				case Bytecode::BC_OP_CAT:
					block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_CONCATENATION;
					break;
				}

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_ADDVN:
				case Bytecode::BC_OP_SUBVN:
				case Bytecode::BC_OP_MULVN:
				case Bytecode::BC_OP_DIVVN:
				case Bytecode::BC_OP_MODVN:
					block[i]->assignment.expressions.back()->binaryOperation->leftOperand = new_slot(block[i]->instruction.b);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->binaryOperation->leftOperand);
					block[i]->assignment.expressions.back()->binaryOperation->rightOperand = new_number(function, block[i]->instruction.c);
					break;
				case Bytecode::BC_OP_ADDNV:
				case Bytecode::BC_OP_SUBNV:
				case Bytecode::BC_OP_MULNV:
				case Bytecode::BC_OP_DIVNV:
				case Bytecode::BC_OP_MODNV:
					block[i]->assignment.expressions.back()->binaryOperation->leftOperand = new_number(function, block[i]->instruction.c);
					block[i]->assignment.expressions.back()->binaryOperation->rightOperand = new_slot(block[i]->instruction.b);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->binaryOperation->rightOperand);
					break;
				case Bytecode::BC_OP_ADDVV:
				case Bytecode::BC_OP_SUBVV:
				case Bytecode::BC_OP_MULVV:
				case Bytecode::BC_OP_DIVVV:
				case Bytecode::BC_OP_MODVV:
				case Bytecode::BC_OP_POW:
					block[i]->assignment.expressions.back()->binaryOperation->leftOperand = new_slot(block[i]->instruction.b);
					block[i]->assignment.expressions.back()->binaryOperation->rightOperand = new_slot(block[i]->instruction.c);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->binaryOperation->leftOperand, block[i]->assignment.expressions.back()->binaryOperation->rightOperand);
					break;
				case Bytecode::BC_OP_CAT:
					block[i]->assignment.expressions.back()->binaryOperation->leftOperand = new_slot(block[i]->instruction.b);

					for (Expression* expression = block[i]->assignment.expressions.back(); true; expression = expression->binaryOperation->rightOperand) {
						block[i]->assignment.register_open_slots(expression->binaryOperation->leftOperand);

						if (expression->binaryOperation->leftOperand->variable->slot == block[i]->instruction.c - 1) {
							expression->binaryOperation->rightOperand = new_slot(block[i]->instruction.c);
							block[i]->assignment.register_open_slots(expression->binaryOperation->rightOperand);
							break;
						}

						expression->binaryOperation->rightOperand = new_expression(AST_EXPRESSION_BINARY_OPERATION);
						expression->binaryOperation->rightOperand->binaryOperation->type = AST_BINARY_CONCATENATION;
						expression->binaryOperation->rightOperand->binaryOperation->leftOperand = new_slot(expression->binaryOperation->leftOperand->variable->slot + 1);
					}

					break;
				}

				break;
			case Bytecode::BC_OP_KSTR:
				block[i]->assignment.expressions.back() = new_string(function, block[i]->instruction.d);
				break;
			case Bytecode::BC_OP_KCDATA:
				block[i]->assignment.expressions.back() = new_cdata(function, block[i]->instruction.d);
				break;
			case Bytecode::BC_OP_KSHORT:
				block[i]->assignment.expressions.back() = new_signed_literal(block[i]->instruction.d);
				break;
			case Bytecode::BC_OP_KNUM:
				block[i]->assignment.expressions.back() = new_number(function, block[i]->instruction.d);
				break;
			case Bytecode::BC_OP_KPRI:
				block[i]->assignment.expressions.back() = new_primitive(block[i]->instruction.d);
				break;
			case Bytecode::BC_OP_KNIL:
				block[i]->assignment.expressions.back() = new_primitive(0);
				if (block[i]->instruction.a == block[i]->instruction.d) break;
				block.emplace(block.begin() + i, new_statement(AST_STATEMENT_INSTRUCTION));
				block[i]->instruction = block[i + 1]->instruction;
				block[i]->instruction.d--;
				i++;
				block[i]->instruction.a = block[i]->instruction.d;
				block[i]->instruction.id = INVALID_ID;
				block[i]->instruction.attachedLabel = INVALID_ID;
				break;
			case Bytecode::BC_OP_UGET:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_VARIABLE);
				block[i]->assignment.expressions.back()->variable->type = AST_VARIABLE_UPVALUE;
				block[i]->assignment.expressions.back()->variable->slotScope = function.upvalues[block[i]->instruction.d].slotScope;
				break;
			case Bytecode::BC_OP_USETV:
			case Bytecode::BC_OP_USETS:
			case Bytecode::BC_OP_USETN:
			case Bytecode::BC_OP_USETP:
				block[i]->assignment.variables.resize(1);
				block[i]->assignment.variables.back().type = AST_VARIABLE_UPVALUE;
				block[i]->assignment.variables.back().slotScope = function.upvalues[block[i]->instruction.a].slotScope;

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_USETV:
					block[i]->assignment.expressions.back() = new_slot(block[i]->instruction.d);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
					break;
				case Bytecode::BC_OP_USETS:
					block[i]->assignment.expressions.back() = new_string(function, block[i]->instruction.d);
					break;
				case Bytecode::BC_OP_USETN:
					block[i]->assignment.expressions.back() = new_number(function, block[i]->instruction.d);
					break;
				case Bytecode::BC_OP_USETP:
					block[i]->assignment.expressions.back() = new_primitive(block[i]->instruction.d);
					break;
				}

				continue;
			case Bytecode::BC_OP_FNEW:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_FUNCTION);
				block[i]->assignment.expressions.back()->function = block[i]->function;
				break;
			case Bytecode::BC_OP_TNEW:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_TABLE);
				block[i]->assignment.isTableConstructor = true;
				break;
			case Bytecode::BC_OP_TDUP:
				block[i]->assignment.expressions.back() = new_table(function, block[i]->instruction.d);
				block[i]->assignment.isTableConstructor = true;
				break;
			case Bytecode::BC_OP_GGET:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_VARIABLE);
				block[i]->assignment.expressions.back()->variable->type = AST_VARIABLE_GLOBAL;
				block[i]->assignment.expressions.back()->variable->name = function.get_constant(block[i]->instruction.d).string;
				if (function.hasDebugInfo) function.usedGlobals.emplace_back(&function.get_constant(block[i]->instruction.d).string);
				break;
			case Bytecode::BC_OP_GSET:
				block[i]->assignment.variables.resize(1);
				block[i]->assignment.variables.back().type = AST_VARIABLE_GLOBAL;
				block[i]->assignment.variables.back().name = function.get_constant(block[i]->instruction.d).string;
				if (function.hasDebugInfo) function.usedGlobals.emplace_back(&function.get_constant(block[i]->instruction.d).string);
				block[i]->assignment.expressions.back() = new_slot(block[i]->instruction.a);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
				continue;
			case Bytecode::BC_OP_TGETV:
			case Bytecode::BC_OP_TGETS:
			case Bytecode::BC_OP_TGETB:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_VARIABLE);
				block[i]->assignment.expressions.back()->variable->type = AST_VARIABLE_TABLE_INDEX;
				block[i]->assignment.expressions.back()->variable->table = new_slot(block[i]->instruction.b);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->variable->table);

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_TGETV:
					block[i]->assignment.expressions.back()->variable->tableIndex = new_slot(block[i]->instruction.c);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->variable->tableIndex);
					break;
				case Bytecode::BC_OP_TGETS:
					block[i]->assignment.expressions.back()->variable->tableIndex = new_string(function, block[i]->instruction.c);
					check_valid_name(block[i]->assignment.expressions.back()->variable->tableIndex->constant);
					break;
				case Bytecode::BC_OP_TGETB:
					block[i]->assignment.expressions.back()->variable->tableIndex = new_literal(block[i]->instruction.c);
					break;
				}

				break;
			case Bytecode::BC_OP_TSETV:
			case Bytecode::BC_OP_TSETS:
			case Bytecode::BC_OP_TSETB:
				block[i]->assignment.variables.resize(1);
				block[i]->assignment.variables.back().type = AST_VARIABLE_TABLE_INDEX;
				block[i]->assignment.variables.back().table = new_slot(block[i]->instruction.b);

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_TSETV:
					block[i]->assignment.variables.back().tableIndex = new_slot(block[i]->instruction.c);
					block[i]->assignment.register_open_slots(block[i]->assignment.variables.back().tableIndex);
					break;
				case Bytecode::BC_OP_TSETS:
					block[i]->assignment.variables.back().tableIndex = new_string(function, block[i]->instruction.c);
					check_valid_name(block[i]->assignment.variables.back().tableIndex->constant);
					break;
				case Bytecode::BC_OP_TSETB:
					block[i]->assignment.variables.back().tableIndex = new_literal(block[i]->instruction.c);
					break;
				}

				block[i]->assignment.expressions.back() = new_slot(block[i]->instruction.a);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
				continue;
			case Bytecode::BC_OP_TSETM:
				block[i]->assignment.variables.resize(1);
				block[i]->assignment.variables.back().type = AST_VARIABLE_TABLE_INDEX;
				block[i]->assignment.variables.back().isMultres = true;
				block[i]->assignment.variables.back().table = new_slot(block[i]->instruction.a - 1);
				assert(function.get_number_constant(block[i]->instruction.d).type == Bytecode::BC_KNUM_NUM, "Multres table index is not a valid number constant", bytecode.filePath, DEBUG_INFO);
				block[i]->assignment.variables.back().multresIndex = function.get_number_constant(block[i]->instruction.d).number;
				block[i]->assignment.expressions.back() = new_slot(block[i]->instruction.a);
				block[i]->assignment.expressions.back()->variable->isMultres = true;
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
				continue;
			case Bytecode::BC_OP_CALLM:
			case Bytecode::BC_OP_CALL:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_FUNCTION_CALL);

				if (block[i]->instruction.b) {
					if (block[i]->instruction.b == 1) {
						block[i]->type = AST_STATEMENT_FUNCTION_CALL;
					} else {
						block[i]->assignment.variables.resize(block[i]->instruction.b - 1);

						for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
							block[i]->assignment.variables[j].type = AST_VARIABLE_SLOT;
							block[i]->assignment.variables[j].slot = block[i]->instruction.a + j;
						}

						block[i]->assignment.expressions.back()->functionCall->returnCount = block[i]->assignment.variables.size();
					}
				} else {
					block[i]->assignment.variables.resize(1);
					block[i]->assignment.variables.back().type = AST_VARIABLE_SLOT;
					block[i]->assignment.variables.back().slot = block[i]->instruction.a;
					block[i]->assignment.variables.back().isMultres = true;
				}

				block[i]->assignment.expressions.back()->functionCall->function = new_slot(block[i]->instruction.a);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->function);
				block[i]->assignment.expressions.back()->functionCall->arguments.resize(block[i]->instruction.c + (block[i]->instruction.type == Bytecode::BC_OP_CALLM ? 0 : -1), nullptr);
				if (block[i]->assignment.expressions.back()->functionCall->arguments.size()) block[i]->assignment.isPotentialMethod = true;

				for (uint8_t j = 0; j < block[i]->assignment.expressions.back()->functionCall->arguments.size(); j++) {
					block[i]->assignment.expressions.back()->functionCall->arguments[j] = new_slot(block[i]->instruction.a + (isFR2Enabled ? 2 : 1) + j);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->arguments[j]);
				}

				if (block[i]->instruction.type == Bytecode::BC_OP_CALLM) {
					block[i]->assignment.expressions.back()->functionCall->multresArgument = new_slot(block[i]->instruction.a + (isFR2Enabled ? 2 : 1) + block[i]->instruction.c);
					block[i]->assignment.expressions.back()->functionCall->multresArgument->variable->isMultres = true;
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->multresArgument);
				}

				continue;
			case Bytecode::BC_OP_VARG:
				block[i]->assignment.expressions.back() = new_expression(AST_EXPRESSION_VARARG);

				if (block[i]->instruction.b) {
					if (block[i]->instruction.b == 1) {
						block[i]->type = AST_STATEMENT_FUNCTION_CALL;
					} else {
						block[i]->assignment.variables.resize(block[i]->instruction.b - 1);

						for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
							block[i]->assignment.variables[j].type = AST_VARIABLE_SLOT;
							block[i]->assignment.variables[j].slot = block[i]->instruction.a + j;
						}

						block[i]->assignment.expressions.back()->returnCount = block[i]->assignment.variables.size();
					}
				} else {
					block[i]->assignment.variables.resize(1);
					block[i]->assignment.variables.back().type = AST_VARIABLE_SLOT;
					block[i]->assignment.variables.back().slot = block[i]->instruction.a;
					block[i]->assignment.variables.back().isMultres = true;
				}

				continue;
			}

			block[i]->assignment.variables.resize(1);
			block[i]->assignment.variables.back().type = AST_VARIABLE_SLOT;
			block[i]->assignment.variables.back().slot = block[i]->instruction.a;
			continue;
		case AST_STATEMENT_RETURN:
			if (i
				&& block[i - 1]->type == AST_STATEMENT_EMPTY
				&& block[i - 1]->instruction.type == Bytecode::BC_OP_UCLO
				&& !function.is_valid_label(block[i]->instruction.attachedLabel)) {
				block[i]->instruction.id = block[i - 1]->instruction.id;
				block[i]->instruction.attachedLabel = block[i - 1]->instruction.attachedLabel;
				i--;
				block.erase(block.begin() + i);
			}

			switch (block[i]->instruction.type) {
			case Bytecode::BC_OP_CALLMT:
			case Bytecode::BC_OP_CALLT:
				block[i]->assignment.expressions.resize(1, new_expression(AST_EXPRESSION_FUNCTION_CALL));
				block[i]->assignment.expressions.back()->functionCall->function = new_slot(block[i]->instruction.a);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->function);
				block[i]->assignment.expressions.back()->functionCall->arguments.resize(block[i]->instruction.d + (block[i]->instruction.type == Bytecode::BC_OP_CALLMT ? 0 : -1), nullptr);
				if (block[i]->assignment.expressions.back()->functionCall->arguments.size()) block[i]->assignment.isPotentialMethod = true;

				for (uint8_t j = 0; j < block[i]->assignment.expressions.back()->functionCall->arguments.size(); j++) {
					block[i]->assignment.expressions.back()->functionCall->arguments[j] = new_slot(block[i]->instruction.a + (isFR2Enabled ? 2 : 1) + j);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->arguments[j]);
				}

				if (block[i]->instruction.type == Bytecode::BC_OP_CALLMT) {
					block[i]->assignment.expressions.back()->functionCall->multresArgument = new_slot(block[i]->instruction.a + (isFR2Enabled ? 2 : 1) + block[i]->instruction.d);
					block[i]->assignment.expressions.back()->functionCall->multresArgument->variable->isMultres = true;
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back()->functionCall->multresArgument);
				}

				break;
			case Bytecode::BC_OP_RETM:
			case Bytecode::BC_OP_RET:
			case Bytecode::BC_OP_RET1:
				block[i]->assignment.expressions.resize(block[i]->instruction.d + (block[i]->instruction.type == Bytecode::BC_OP_RETM ? 0 : -1), nullptr);

				for (uint8_t j = 0; j < block[i]->assignment.expressions.size(); j++) {
					block[i]->assignment.expressions[j] = new_slot(block[i]->instruction.a + j);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions[j]);
				}

				if (block[i]->instruction.type == Bytecode::BC_OP_RETM) {
					block[i]->assignment.multresReturn = new_slot(block[i]->instruction.a + block[i]->instruction.d);
					block[i]->assignment.multresReturn->variable->isMultres = true;
					block[i]->assignment.register_open_slots(block[i]->assignment.multresReturn);
				}

				break;
			}

			continue;
		case AST_STATEMENT_CONDITION:
			switch (block[i]->instruction.type) {
			case Bytecode::BC_OP_ISLT:
			case Bytecode::BC_OP_ISGE:
			case Bytecode::BC_OP_ISLE:
			case Bytecode::BC_OP_ISGT:
			case Bytecode::BC_OP_ISEQV:
			case Bytecode::BC_OP_ISNEV:
			case Bytecode::BC_OP_ISEQS:
			case Bytecode::BC_OP_ISNES:
			case Bytecode::BC_OP_ISEQN:
			case Bytecode::BC_OP_ISNEN:
			case Bytecode::BC_OP_ISEQP:
			case Bytecode::BC_OP_ISNEP:
				block[i]->assignment.expressions.resize(2, nullptr);
				block[i]->assignment.expressions[0] = new_slot(block[i]->instruction.a);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions[0]);

				switch (block[i]->instruction.type) {
				case Bytecode::BC_OP_ISLT:
				case Bytecode::BC_OP_ISGE:
				case Bytecode::BC_OP_ISLE:
				case Bytecode::BC_OP_ISGT:
					block[i]->condition.allowSlotSwap = true;
				case Bytecode::BC_OP_ISEQV:
				case Bytecode::BC_OP_ISNEV:
					block[i]->assignment.expressions[1] = new_slot(block[i]->instruction.d);
					block[i]->assignment.register_open_slots(block[i]->assignment.expressions[1]);
					break;
				case Bytecode::BC_OP_ISEQS:
				case Bytecode::BC_OP_ISNES:
					block[i]->assignment.expressions[1] = new_string(function, block[i]->instruction.d);
					break;
				case Bytecode::BC_OP_ISEQN:
				case Bytecode::BC_OP_ISNEN:
					block[i]->assignment.expressions[1] = new_number(function, block[i]->instruction.d);
					break;
				case Bytecode::BC_OP_ISEQP:
				case Bytecode::BC_OP_ISNEP:
					block[i]->assignment.expressions[1] = new_primitive(block[i]->instruction.d);
					break;
				}

				break;
			case Bytecode::BC_OP_ISTC:
			case Bytecode::BC_OP_ISFC:
				block[i]->assignment.variables.resize(1);
				block[i]->assignment.variables.back().type = AST_VARIABLE_SLOT;
				block[i]->assignment.variables.back().slot = block[i]->instruction.a;
			case Bytecode::BC_OP_IST:
			case Bytecode::BC_OP_ISF:
				block[i]->assignment.expressions.resize(1, new_slot(block[i]->instruction.d));
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions.back());
				block[i]->assignment.allowedConstantType = INVALID_CONSTANT;
				break;
			}

			continue;
		case AST_STATEMENT_NUMERIC_FOR:
			block[i]->assignment.variables.resize(1);
			block[i]->assignment.variables.back().type = AST_VARIABLE_SLOT;
			block[i]->assignment.variables.back().slot = block[i]->instruction.a + 3;
			assert(!function.hasDebugInfo || block[i]->locals && block[i]->assignment.variables.back().slot == block[i]->locals->baseSlot && block[i]->locals->names.size() == 1,
				"Numeric for loop variable does not match with debug info", bytecode.filePath, DEBUG_INFO);
			block[i]->assignment.expressions.resize(3, nullptr);
			block[i]->assignment.expressions[0] = new_slot(block[i]->instruction.a);
			block[i]->assignment.expressions[1] = new_slot(block[i]->instruction.a + 1);
			block[i]->assignment.expressions[2] = new_slot(block[i]->instruction.a + 2);
			block[i]->assignment.register_open_slots(block[i]->assignment.expressions[0], block[i]->assignment.expressions[1], block[i]->assignment.expressions[2]);
			continue;
		case AST_STATEMENT_GENERIC_FOR:
			block[i]->assignment.variables.resize(block[i]->instruction.b - 1);

			for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
				block[i]->assignment.variables[j].type = AST_VARIABLE_SLOT;
				block[i]->assignment.variables[j].slot = block[i]->instruction.a + j;
			}

			assert(!function.hasDebugInfo || block[i]->locals && block[i]->assignment.variables.front().slot == block[i]->locals->baseSlot && block[i]->locals->names.size() == block[i]->assignment.variables.size(),
				"Generic for loop variables do not match with debug info", bytecode.filePath, DEBUG_INFO);
			block[i]->assignment.expressions.resize(3, nullptr);
			block[i]->assignment.expressions[0] = new_slot(block[i]->instruction.a - 3);
			block[i]->assignment.expressions[1] = new_slot(block[i]->instruction.a - 2);
			block[i]->assignment.expressions[2] = new_slot(block[i]->instruction.a - 1);
			block[i]->assignment.register_open_slots(block[i]->assignment.expressions[0], block[i]->assignment.expressions[1], block[i]->assignment.expressions[2]);
			continue;
		case AST_STATEMENT_DECLARATION:
			block[i]->assignment.variables.resize(block[i]->locals->names.size());
			block[i]->assignment.expressions.resize(block[i]->assignment.variables.size(), nullptr);

			for (uint8_t j = 0; j < block[i]->assignment.variables.size(); j++) {
				block[i]->assignment.variables[j].type = AST_VARIABLE_SLOT;
				block[i]->assignment.variables[j].slot = block[i]->locals->baseSlot + j;
				block[i]->assignment.expressions[j] = new_slot(block[i]->assignment.variables[j].slot);
				block[i]->assignment.register_open_slots(block[i]->assignment.expressions[j]);
			}

			continue;
		}
	}
}

void Ast::collect_slot_scopes(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	static Statement* const (* const build_nil_assignment)(Ast& ast, const uint8_t& slot) = [](Ast& ast, const uint8_t& slot)->Statement* const {
		Statement* const statement = ast.new_statement(AST_STATEMENT_ASSIGNMENT);
		statement->assignment.expressions.resize(1, ast.new_primitive(0));
		statement->assignment.variables.resize(1);
		statement->assignment.variables.back().type = AST_VARIABLE_SLOT;
		statement->assignment.variables.back().slot = slot;
		return statement;
	};

	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	uint32_t id, index, targetLabel, extendedTargetLabel;
	uint8_t targetSlot;
	SlotScope** targetSlotScope;
	bool isPossibleCondition;
	bool hasBoolConstruct;
	std::vector<std::vector<Statement*>> conditionBlocks;

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			for (uint32_t j = block[i]->assignment.variables.size(); j--;) {
				assert(!function.slotScopeCollector.slotInfos[block[i]->assignment.variables[j].slot].activeSlotScope, "Slot scope does not match with for loop variable", bytecode.filePath, DEBUG_INFO);
				function.slotScopeCollector.begin_scope(block[i]->assignment.variables[j].slot, block[i]->instruction.target - 1);
			}
		case AST_STATEMENT_LOOP:
			function.slotScopeCollector.extend_scopes(block[i]->instruction.id);
			blockInfo.index = i;
			collect_slot_scopes(function, block[i]->block, &blockInfo);
			function.slotScopeCollector.merge_scopes(block[i]->instruction.target);
			break;
		case AST_STATEMENT_DECLARATION:
			block[i]->instruction.id = INVALID_ID;

			for (uint8_t j = function.slotScopeCollector.slotInfos.size(); j-- && j >= block[i]->locals->baseSlot;) {
				if (!function.slotScopeCollector.slotInfos[j].activeSlotScope) continue;

				for (uint8_t k = j; true; k--) {
					assert(function.slotScopeCollector.slotInfos[k].activeSlotScope && function.slotScopeCollector.slotInfos[k].minScopeBegin == INVALID_ID,
						"Slot scope does not match with variable debug info", bytecode.filePath, DEBUG_INFO);
					block.emplace(block.begin() + i + 1, build_nil_assignment(*this, k));
					function.slotScopeCollector.complete_scope(k, block[i + 1]->assignment.variables.back().slotScope, block[i]->locals->scopeEnd);
					if (k == block[i]->locals->baseSlot) break;
				}

				break;
			}

			for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
				function.slotScopeCollector.begin_scope(block[i]->assignment.variables[j].slot, block[i]->locals->scopeEnd);
			}

			function.slotScopeCollector.extend_scopes(block[i]->locals->scopeBegin);
			blockInfo.index = i;
			collect_slot_scopes(function, block[i]->block, &blockInfo);

			for (uint8_t j = function.slotScopeCollector.slotInfos.size(); j-- && j >= block[i]->assignment.variables.back().slot + 1;) {
				if (!function.slotScopeCollector.slotInfos[j].activeSlotScope) continue;

				for (uint8_t k = j; true; k--) {
					assert(function.slotScopeCollector.slotInfos[k].activeSlotScope && function.slotScopeCollector.slotInfos[k].minScopeBegin == INVALID_ID,
						"Slot scope does not match with variable debug info", bytecode.filePath, DEBUG_INFO);
					block[i]->block.emplace(block[i]->block.begin(), build_nil_assignment(*this, k));
					function.slotScopeCollector.complete_scope(k, block[i]->block.front()->assignment.variables.back().slotScope, block[i]->locals->scopeBegin);
					if (k == block[i]->assignment.variables.back().slot + 1) break;
				}

				break;
			}

			break;
		}

		if (block[i]->instruction.id != INVALID_ID) {
			id = block[i]->instruction.id;
			blockInfo.index = i;
			targetLabel = get_label_from_next_statement(function, blockInfo, false, true);
			extendedTargetLabel = get_label_from_next_statement(function, blockInfo, true, true);

			if (function.is_valid_label(targetLabel)
				&& function.labels[targetLabel].jumpIds.front() < id
				&& (extendedTargetLabel == targetLabel
					|| function.labels[extendedTargetLabel].target > id
					|| function.labels[extendedTargetLabel].target < function.labels[targetLabel].jumpIds.front())) {
				index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds.front() - 1);

				if (index != INVALID_ID) {
					isPossibleCondition = false;
					hasBoolConstruct = false;

					switch (block[i]->type) {
					case AST_STATEMENT_CONDITION:
						if (!block[i]->assignment.variables.size() && block[i]->instruction.target == function.labels[extendedTargetLabel].target) {
							switch (block[index]->type) {
							case AST_STATEMENT_CONDITION:
								if (block[index]->assignment.expressions.size() == 1) {
									if (block[index]->assignment.variables.size()) {
										if (function.slotScopeCollector.slotInfos[block[index]->assignment.variables.back().slot].activeSlotScope
											&& function.slotScopeCollector.slotInfos[block[index]->assignment.variables.back().slot].minScopeBegin == block[index]->instruction.id) {
											isPossibleCondition = true;
											targetSlot = block[index]->assignment.variables.back().slot;
										}
									} else if (function.slotScopeCollector.slotInfos[block[index]->assignment.expressions.back()->variable->slot].activeSlotScope
										&& function.slotScopeCollector.slotInfos[block[index]->assignment.expressions.back()->variable->slot].minScopeBegin == block[index]->instruction.id) {
										isPossibleCondition = true;
										targetSlot = block[index]->assignment.expressions.back()->variable->slot;
									}
								}

								break;
							case AST_STATEMENT_ASSIGNMENT:
								if (block[index]->assignment.variables.size() == 1
									&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
									&& function.slotScopeCollector.slotInfos[block[index]->assignment.variables.back().slot].activeSlotScope
									&& function.slotScopeCollector.slotInfos[block[index]->assignment.variables.back().slot].minScopeBegin == block[index]->instruction.id
									&& get_constant_type(block[index]->assignment.expressions.back())) {
									isPossibleCondition = true;
									targetSlot = block[index]->assignment.variables.back().slot;
								}

								break;
							}
						}

						break;
					case AST_STATEMENT_ASSIGNMENT:
						if (block[i]->assignment.variables.size() == 1) {
							switch (block[i]->assignment.variables.back().type) {
							case AST_VARIABLE_SLOT:
								if (function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].activeSlotScope
									&& function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].minScopeBegin == block[index]->instruction.id) {
									isPossibleCondition = true;
									targetSlot = block[i]->assignment.variables.back().slot;
									if (i >= 5
										&& index <= i - 4
										&& (((block[i - 3]->type == AST_STATEMENT_GOTO
													|| block[i - 3]->type == AST_STATEMENT_BREAK)
												&& block[i - 3]->instruction.target == function.labels[extendedTargetLabel].target)
											|| (block[i - 3]->type == AST_STATEMENT_CONDITION
												&& block[i - 3]->assignment.expressions.size() == 2
												&& block[i - 3]->instruction.target == block[i]->instruction.id))
										&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
										&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_TRUE
										&& (block[i - 1]->type == AST_STATEMENT_GOTO
											|| block[i - 1]->type == AST_STATEMENT_BREAK)
										&& block[i - 1]->instruction.target == function.labels[targetLabel].target
										&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
										&& block[i - 2]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
										&& block[i - 2]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE
										&& (function.is_valid_label(block[i]->instruction.attachedLabel)
											|| function.is_valid_label(block[i - 2]->instruction.attachedLabel)))
										hasBoolConstruct = true;
								}

								break;
							case AST_VARIABLE_TABLE_INDEX:
								if (function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().table->variable->slot].activeSlotScope
									&& function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().table->variable->slot].minScopeBegin == block[index]->instruction.id) {
									isPossibleCondition = true;
									targetSlot = block[i]->assignment.variables.back().table->variable->slot;
								}

								break;
							}
						}

						break;
					}

					if (isPossibleCondition) {
						conditionBlocks.clear();

						if (hasBoolConstruct) {
							conditionBlocks.resize(2);
							conditionBlocks[0].emplace_back(block[i]);
							conditionBlocks[1].emplace_back(block[i - 2]);
							conditionBlocks[1].emplace_back(block[i - 1]);
							index = block[i - 3]->type == AST_STATEMENT_CONDITION ? i - 3 : i - 4;
						} else {
							index = i;
						}

						if (!hasBoolConstruct || index == i - 4) {
							isPossibleCondition = false;

							if (block[index]->type == AST_STATEMENT_ASSIGNMENT
								&& block[index]->assignment.variables.size() == 1
								&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT) {
								if (block[index]->assignment.variables.back().slot == targetSlot) isPossibleCondition = true;
							} else if ((block[index]->type == AST_STATEMENT_ASSIGNMENT
								&& block[index]->assignment.variables.size() == 1
								&& block[index]->assignment.variables.back().type == AST_VARIABLE_TABLE_INDEX
								&& block[index]->assignment.variables.back().table->variable->slot == targetSlot)
								|| (block[index]->type == AST_STATEMENT_CONDITION
									&& block[index]->instruction.target == function.labels[extendedTargetLabel].target
									&& !block[index]->assignment.variables.size())) {
								while (index--) {
									switch (block[index]->type) {
									case AST_STATEMENT_CONDITION:
										if (!block[index]->assignment.variables.size() && block[index]->instruction.target == function.labels[extendedTargetLabel].target) continue;
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										if (block[index]->instruction.target == function.labels[targetLabel].target
											|| block[index]->instruction.target == function.labels[extendedTargetLabel].target
											|| block[index]->instruction.target > block[hasBoolConstruct ? i - 4 : i]->instruction.id)
											break;
										continue;
									case AST_STATEMENT_ASSIGNMENT:
										if (block[index]->assignment.variables.size() == 1
											&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
											&& block[index]->assignment.variables.back().slot == targetSlot) {
											if (block[index]->assignment.isTableConstructor
												&& (hasBoolConstruct
													|| block[index]->instruction.id > function.labels[targetLabel].jumpIds.front())
												&& function.is_valid_block_range(block[index + 1]->instruction.id, block[hasBoolConstruct ? i - 4 : i]->instruction.id))
												isPossibleCondition = true;
											break;
										}
									default:
										continue;
									}

									break;
								}
							}
						}

						for (uint32_t blockIndex = hasBoolConstruct ? i - 3 : i; isPossibleCondition;) {
							if (block[index]->instruction.id < function.labels[targetLabel].jumpIds.front()) {
								conditionBlocks.emplace_back(block.begin() + index, block.begin() + blockIndex + 1);
								break;
							}

							isPossibleCondition = false;

							while (index--) {
								switch (block[index]->type) {
									case AST_STATEMENT_CONDITION:
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										if (block[index]->instruction.target == function.labels[targetLabel].target) break;
									default:
										continue;
								}

								conditionBlocks.emplace_back(block.begin() + index + 1, block.begin() + blockIndex + 1);
								blockIndex = index;

								switch (block[index]->type) {
								case AST_STATEMENT_CONDITION:
									if (block[index]->assignment.expressions.size() != 1) break;

									if (block[index]->assignment.variables.size()) {
										if (block[index]->assignment.variables.back().slot == targetSlot) isPossibleCondition = true;
									} else if (block[index]->assignment.expressions.back()->variable->slot == targetSlot) {
										index--;

										if (block[index]->type == AST_STATEMENT_ASSIGNMENT
											&& block[index]->assignment.variables.size() == 1
											&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT) {
											if (block[index]->assignment.variables.back().slot == targetSlot && !function.is_valid_label(block[index + 1]->instruction.attachedLabel)) isPossibleCondition = true;
										} else if ((block[index]->type == AST_STATEMENT_ASSIGNMENT
												&& block[index]->assignment.variables.size() == 1
												&& block[index]->assignment.variables.back().type == AST_VARIABLE_TABLE_INDEX
												&& block[index]->assignment.variables.back().table->variable->slot == targetSlot
												&& !function.is_valid_label(block[index + 1]->instruction.attachedLabel))
											|| (block[index]->type == AST_STATEMENT_CONDITION
												&& block[index]->instruction.target == block[blockIndex]->instruction.id
												&& !block[index]->assignment.variables.size())) {
											while (index--) {
												switch (block[index]->type) {
												case AST_STATEMENT_CONDITION:
													if (!block[index]->assignment.variables.size() && block[index]->instruction.target == block[blockIndex]->instruction.id) continue;
												case AST_STATEMENT_GOTO:
												case AST_STATEMENT_BREAK:
													if (block[index]->instruction.target == function.labels[targetLabel].target
														|| block[index]->instruction.target == function.labels[extendedTargetLabel].target
														|| block[index]->instruction.target >= block[blockIndex]->instruction.id) break;
													continue;
												case AST_STATEMENT_ASSIGNMENT:
													if (block[index]->assignment.variables.size() == 1
														&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
														&& block[index]->assignment.variables.back().slot == targetSlot) {
														if (block[index]->assignment.isTableConstructor
															&& function.is_valid_block_range(block[index + 1]->instruction.id, block[blockIndex]->instruction.id))
															isPossibleCondition = true;
														break;
													}
												default:
													continue;
												}

												break;
											}
										}
									}

									break;
								case AST_STATEMENT_GOTO:
								case AST_STATEMENT_BREAK:
									index--;
									if (block[index]->assignment.variables.size() == 1
										&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
										&& block[index]->assignment.variables.back().slot == targetSlot
										&& get_constant_type(block[index]->assignment.expressions.back()))
										isPossibleCondition = true;
									break;
								}

								break;
							}
						}

						if (isPossibleCondition) {
							for (uint32_t j = index; j <= i; j++) {
								switch (block[j]->type) {
								case AST_STATEMENT_ASSIGNMENT:
									if (block[j]->assignment.variables.size() == 1) {
										switch (block[j]->assignment.variables.back().type) {
										case AST_VARIABLE_SLOT:
										case AST_VARIABLE_TABLE_INDEX:
											continue;
										}
									}
								case AST_STATEMENT_EMPTY:
								case AST_STATEMENT_RETURN:
								case AST_STATEMENT_NUMERIC_FOR:
								case AST_STATEMENT_GENERIC_FOR:
								case AST_STATEMENT_LOOP:
								case AST_STATEMENT_DECLARATION:
								case AST_STATEMENT_FUNCTION_CALL:
									break;
								case AST_STATEMENT_GOTO:
								case AST_STATEMENT_BREAK:
									if (block[j]->instruction.type != Bytecode::BC_OP_JMP) break;
								case AST_STATEMENT_CONDITION:
									if (block[j]->instruction.target != function.labels[targetLabel].target
										&& block[j]->instruction.target != function.labels[extendedTargetLabel].target
										&& (block[j]->instruction.target > id
											|| block[j]->instruction.target <= block[j]->instruction.id))
										break;
								default:
									continue;
								}

								isPossibleCondition = false;
								break;
							}

							if (isPossibleCondition) {
								for (uint32_t j = conditionBlocks.size(); j--;) {
									if ((conditionBlocks[j].size() > 1
											&& !function.is_valid_block_range(conditionBlocks[j][1]->instruction.id, conditionBlocks[j].back()->instruction.id)
										|| (function.is_valid_label(conditionBlocks[j].front()->instruction.attachedLabel)
											&& function.labels[conditionBlocks[j].front()->instruction.attachedLabel].jumpIds.back() >= conditionBlocks[j].front()->instruction.id))) {
										isPossibleCondition = false;
										break;
									}
								}

								if (isPossibleCondition) {
									targetSlotScope = function.slotScopeCollector.slotInfos[targetSlot].activeSlotScope;
									function.slotScopeCollector.slotInfos[targetSlot].minScopeBegin = INVALID_ID;
									i++;

									for (uint32_t j = 0; j < conditionBlocks.size(); j++) {
										if (j
											&& (!hasBoolConstruct
												|| j != 2
												|| conditionBlocks[j].back()->type != AST_STATEMENT_CONDITION)) {
											(*targetSlotScope)->usages++;
											function.slotScopeCollector.slotInfos[targetSlot].activeSlotScope = targetSlotScope;
										}

										collect_slot_scopes(function, conditionBlocks[j]);
										i -= conditionBlocks[j].size();
										if (!function.slotScopeCollector.slotInfos[targetSlot].activeSlotScope || j == conditionBlocks.size() - 1) continue;

										while (function.slotScopeCollector.slotInfos[targetSlot].slotScopes.back() != targetSlotScope) {
											(*targetSlotScope)->usages += (*function.slotScopeCollector.slotInfos[targetSlot].slotScopes.back())->usages + 1;
											function.slotScopeCollector.slotInfos[targetSlot].slotScopes.pop_back();
										}

										function.slotScopeCollector.slotInfos[targetSlot].activeSlotScope = targetSlotScope;
										function.slotScopeCollector.slotInfos[targetSlot].minScopeBegin = function.get_scope_begin_from_label(targetLabel, (*targetSlotScope)->scopeEnd);
										break;
									}

									continue;
								}
							}
						}
					}
				}
			}
		} else {
			id = function.slotScopeCollector.previousId - 1;
		}

		function.slotScopeCollector.begin_upvalue_scopes(id);

		if (block[i]->function) {
			for (uint8_t j = block[i]->function->upvalues.size(); j--;) {
				if (!block[i]->function->upvalues[j].local) continue;
				if (block[i]->function->upvalues[j].slot == block[i]->assignment.variables.back().slot) block[i]->function->assignmentSlotIsUpvalue = true;
				block[i]->assignment.usedSlots.emplace_back(block[i]->function->upvalues[j].slot);
				function.slotScopeCollector.add_to_scope(block[i]->function->upvalues[j].slot, block[i]->function->upvalues[j].slotScope, id);
			}
		}

		for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
			switch (block[i]->assignment.variables[j].type) {
			case AST_VARIABLE_SLOT:
				function.slotScopeCollector.complete_scope(block[i]->assignment.variables[j].slot, block[i]->assignment.variables[j].slotScope, id);
				continue;
			case AST_VARIABLE_TABLE_INDEX:
				function.slotScopeCollector.add_to_scope(block[i]->assignment.variables[j].table->variable->slot, block[i]->assignment.variables[j].table->variable->slotScope, id);
				continue;
			}
		}

		assert(!block[i]->assignment.variables.size()
			|| block[i]->assignment.variables.front().type != AST_VARIABLE_SLOT
			|| !block[i]->assignment.variables.front().isMultres
			|| ((*block[i]->assignment.variables.front().slotScope)->usages == 1
				&& (!function.slotScopeCollector.slotInfos[block[i]->assignment.variables.front().slot].activeSlotScope
					|| *function.slotScopeCollector.slotInfos[block[i]->assignment.variables.front().slot].activeSlotScope != *block[i]->assignment.variables.front().slotScope)),
			"Multres assignment has invalid number of usages", bytecode.filePath, DEBUG_INFO);

		if (block[i]->type == AST_STATEMENT_DECLARATION && function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].activeSlotScope) {
			index = function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].minScopeBegin;
			function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].minScopeBegin = INVALID_ID;
			function.slotScopeCollector.complete_scope(block[i]->assignment.variables.back().slot, block[i]->assignment.variables.back().slotScope, id);
			(*block[i]->assignment.variables.back().slotScope)->usages--;
			function.slotScopeCollector.slotInfos[block[i]->assignment.variables.back().slot].minScopeBegin = index;
		}

		for (uint8_t j = block[i]->assignment.openSlots.size(); j--;) {
			function.slotScopeCollector.add_to_scope((*block[i]->assignment.openSlots[j])->variable->slot, (*block[i]->assignment.openSlots[j])->variable->slotScope, id);
		}

		if (block[i]->instruction.id != INVALID_ID) {
			function.slotScopeCollector.previousId = id;

			if (function.is_valid_label(block[i]->instruction.attachedLabel)) {
				id = function.get_scope_end_from_label(block[i]->instruction.attachedLabel);
				function.slotScopeCollector.merge_scopes(id);
				function.slotScopeCollector.extend_scopes(function.get_scope_begin_from_label(block[i]->instruction.attachedLabel, id));
			}
		}
	}
}

void Ast::eliminate_slots(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	Expression* expression;
	uint32_t index, targetIndex, targetLabel, extendedTargetLabel;
	bool hasBoolConstruct;

	for (uint32_t i = 0; i < block.size(); i++) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			if (block[i]->condition.allowSlotSwap
				&& i
				&& !function.is_valid_label(block[i]->instruction.attachedLabel)
				&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 1]->assignment.variables.size() == 1
				&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
				&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.expressions[0]->variable->slot) {
				expression = block[i]->assignment.expressions[0];
				block[i]->assignment.expressions[0] = block[i]->assignment.expressions[1];
				block[i]->assignment.expressions[1] = expression;
				block[i]->condition.swapped = true;
			}

			break;
		case AST_STATEMENT_GENERIC_FOR:
		case AST_STATEMENT_DECLARATION:
			while (i && !function.is_valid_label(block[i]->instruction.attachedLabel)) {
				switch (block[i - 1]->type) {
				case AST_STATEMENT_ASSIGNMENT:
					if (block[i - 1]->assignment.variables.front().slot <= block[i]->assignment.expressions[block[i]->assignment.openSlots.size() - 1]->variable->slot) break;
					assert(block[i - 1]->assignment.variables.size() == 1 && !(*block[i - 1]->assignment.variables.back().slotScope)->usages, "Invalid expression list assignment", bytecode.filePath, DEBUG_INFO);
				case AST_STATEMENT_FUNCTION_CALL:
					block[i]->assignment.expressions.emplace(block[i]->assignment.expressions.begin() + block[i]->assignment.openSlots.size(), block[i - 1]->assignment.expressions.back());
					block[i]->assignment.usedSlots.reserve(block[i]->assignment.usedSlots.size() + block[i - 1]->assignment.usedSlots.size());
					block[i]->assignment.usedSlots.insert(block[i]->assignment.usedSlots.end(), block[i - 1]->assignment.usedSlots.begin(), block[i - 1]->assignment.usedSlots.end());
					block[i]->instruction.attachedLabel = block[i - 1]->instruction.attachedLabel;
					i--;
					block.erase(block.begin() + i);
					continue;
				}

				if (block[i - 1]->type == AST_STATEMENT_ASSIGNMENT && block[i - 1]->assignment.variables.size() != 1) {
					assert(block[i]->assignment.expressions.size() == block[i]->assignment.openSlots.size()
						&& block[i]->assignment.expressions.back()->variable->slot == block[i - 1]->assignment.variables.back().slot,
						"Invalid multres expression list assignment", bytecode.filePath, DEBUG_INFO);

					while (true) {
						function.slotScopeCollector.remove_scope(block[i]->assignment.expressions.back()->variable->slot, block[i]->assignment.expressions.back()->variable->slotScope);
						block[i]->assignment.openSlots.pop_back();

						if (block[i]->assignment.expressions.back()->variable->slot != block[i - 1]->assignment.variables.front().slot) {
							block[i]->assignment.expressions.pop_back();
							continue;
						}

						block[i]->assignment.expressions.back() = block[i - 1]->assignment.expressions.back();
						block[i]->assignment.usedSlots.reserve(block[i]->assignment.usedSlots.size() + block[i - 1]->assignment.usedSlots.size());
						block[i]->assignment.usedSlots.insert(block[i]->assignment.usedSlots.end(), block[i - 1]->assignment.usedSlots.begin(), block[i - 1]->assignment.usedSlots.end());
						block[i]->instruction.attachedLabel = block[i - 1]->instruction.attachedLabel;
						i--;
						block.erase(block.begin() + i);
						break;
					}
				}

				for (uint32_t j = block[i]->assignment.openSlots.size(); j--;) {
					block[i]->assignment.openSlots[j] = &block[i]->assignment.expressions[j];
				}

				break;
			}

			break;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.back().type == AST_VARIABLE_TABLE_INDEX
				&& !block[i]->assignment.variables.back().isMultres
				&& i >= 3
				&& !function.is_valid_label(block[i]->instruction.attachedLabel)
				&& !function.is_valid_label(block[i - 1]->instruction.attachedLabel)
				&& !function.is_valid_label(block[i - 2]->instruction.attachedLabel)
				&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 1]->assignment.variables.size() == 1
				&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
				&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.variables.back().tableIndex->variable->slot
				&& get_constant_type(block[i - 1]->assignment.expressions.back())
				&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 2]->assignment.variables.size() == 1
				&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[i - 2]->assignment.variables.back().slotScope)->usages == 1
				&& block[i - 2]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->variable->slot
				&& !get_constant_type(block[i - 2]->assignment.expressions.back())
				&& block[i - 3]->assignment.isTableConstructor
				&& block[i - 3]->assignment.variables.back().slot == block[i]->assignment.variables.back().table->variable->slot
				&& !block[i - 3]->assignment.expressions.back()->table->multresField) {
				block[i]->assignment.openSlots[0] = &block[i]->assignment.expressions.back();
				block[i]->assignment.openSlots[1] = &block[i]->assignment.variables.back().tableIndex;
			}

			break;
		}

		for (uint8_t j = block[i]->assignment.openSlots.size();
			j--
			&& i
			&& !function.is_valid_label(block[i]->instruction.attachedLabel)
			&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i - 1]->assignment.variables.size() == 1
			&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1;) {
			if (j == 1
				&& block[i]->assignment.isPotentialMethod
				&& i >= 2
				&& !function.is_valid_label(block[i - 1]->instruction.attachedLabel)
				&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->functionCall->function->variable->slot
				&& block[i - 1]->assignment.usedSlots.size() == 1
				&& block[i - 1]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
				&& block[i - 1]->assignment.expressions.back()->variable->type == AST_VARIABLE_TABLE_INDEX
				&& block[i - 1]->assignment.expressions.back()->variable->table->type == AST_EXPRESSION_VARIABLE
				&& block[i - 1]->assignment.expressions.back()->variable->table->variable->type == AST_VARIABLE_SLOT
				&& block[i - 1]->assignment.expressions.back()->variable->tableIndex->type == AST_EXPRESSION_CONSTANT
				&& block[i - 1]->assignment.expressions.back()->variable->tableIndex->constant->type == AST_CONSTANT_STRING
				&& block[i - 1]->assignment.expressions.back()->variable->tableIndex->constant->isName
				&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 2]->assignment.variables.size() == 1
				&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[i - 2]->assignment.variables.back().slotScope)->usages == 1
				&& block[i - 2]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->functionCall->arguments.front()->variable->slot
				&& block[i - 2]->assignment.usedSlots.size() == 1
				&& block[i - 2]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
				&& block[i - 2]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
				&& block[i - 2]->assignment.expressions.back()->variable->slot == block[i - 1]->assignment.expressions.back()->variable->table->variable->slot) {
				block[i]->assignment.expressions.back()->functionCall->isMethod = true;
				block[i]->assignment.expressions.back()->functionCall->arguments.erase(block[i]->assignment.expressions.back()->functionCall->arguments.begin());
				block[i]->assignment.openSlots.erase(block[i]->assignment.openSlots.begin() + j);
				block[i]->assignment.openSlots.emplace(block[i]->assignment.openSlots.begin(), &block[i - 1]->assignment.expressions.back()->variable->table);
				function.slotScopeCollector.remove_scope(block[i - 2]->assignment.variables.back().slot, block[i - 2]->assignment.variables.back().slotScope);
				block[i - 1]->instruction.attachedLabel = block[i - 2]->instruction.attachedLabel;
				(*block[i - 2]->assignment.expressions.back()->variable->slotScope)->usages--;
				i--;
				block.erase(block.begin() + i - 1);
			}

			if (block[i - 1]->assignment.variables.back().slot != (*block[i]->assignment.openSlots[j])->variable->slot) continue;
			assert(block[i - 1]->assignment.variables.back().isMultres == (*block[i]->assignment.openSlots[j])->variable->isMultres,
				"Multres type mismatch when trying to eliminate slot", bytecode.filePath, DEBUG_INFO);
			expression = *block[i]->assignment.openSlots[j];
			*block[i]->assignment.openSlots[j] = block[i - 1]->assignment.expressions.back();

			if (!j
				&& block[i]->assignment.allowedConstantType != NUMBER_CONSTANT
				&& get_constant_type(block[i]->assignment.expressions.back()) > block[i]->assignment.allowedConstantType) {
				*block[i]->assignment.openSlots[j] = expression;
				break;
			}

			function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
			block[i]->assignment.usedSlots.reserve(block[i]->assignment.usedSlots.size() + block[i - 1]->assignment.usedSlots.size());
			block[i]->assignment.usedSlots.insert(block[i]->assignment.usedSlots.end(), block[i - 1]->assignment.usedSlots.begin(), block[i - 1]->assignment.usedSlots.end());
			block[i]->instruction.attachedLabel = block[i - 1]->instruction.attachedLabel;
			i--;
			block.erase(block.begin() + i);
		}

		assert(!block[i]->assignment.openSlots.size()
			|| (*block[i]->assignment.openSlots.back())->type != AST_EXPRESSION_VARIABLE
			|| !(*block[i]->assignment.openSlots.back())->variable->isMultres,
			"Unable to eliminate multres slot", bytecode.filePath, DEBUG_INFO);

		switch (block[i]->type) {
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			eliminate_slots(function, block[i]->block, &blockInfo);
			break;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() == 1) {
				switch (block[i]->assignment.variables.back().type) {
				case AST_VARIABLE_SLOT:
					if (block[i]->instruction.id == INVALID_ID) break;
					blockInfo.index = i;
					targetLabel = get_label_from_next_statement(function, blockInfo, false, true);
					extendedTargetLabel = get_label_from_next_statement(function, blockInfo, true, true);
					if (!function.is_valid_label(targetLabel) || function.labels[targetLabel].jumpIds.front() > block[i]->instruction.id) break;

					if ((*block[i]->assignment.variables.back().slotScope)->usages >= 2) {
						if ((*block[i]->assignment.variables.back().slotScope)->scopeBegin >= function.labels[targetLabel].jumpIds.front()
							|| (extendedTargetLabel != targetLabel
								&& (function.labels[extendedTargetLabel].target <= block[i]->instruction.id
									|| function.labels[extendedTargetLabel].target >= function.labels[targetLabel].jumpIds.front())))
							break;
						index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds.front() - 1);
						if (index == INVALID_ID) break;

						switch (block[index]->type) {
						case AST_STATEMENT_CONDITION:
							if (block[index]->assignment.variables.size()) {
								if ((*block[index]->assignment.variables.back().slotScope)->scopeBegin == block[index]->instruction.id
									&& *block[index]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope)
									break;
							} else if (index
									&& block[index]->assignment.expressions.size() == 1
									&& !function.is_valid_label(block[index]->instruction.attachedLabel)
									&& block[index - 1]->type == AST_STATEMENT_ASSIGNMENT
									&& block[index - 1]->assignment.variables.size() == 1
									&& block[index - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
									&& (*block[index - 1]->assignment.variables.back().slotScope)->scopeBegin == block[index - 1]->instruction.id
									&& *block[index - 1]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope) {
								break;
							}

							index = INVALID_ID;
							break;
						case AST_STATEMENT_ASSIGNMENT:
							if (block[index]->assignment.variables.size() != 1
								|| block[index]->assignment.variables.back().type != AST_VARIABLE_SLOT
								|| (*block[index]->assignment.variables.back().slotScope)->scopeBegin != block[index]->instruction.id
								|| *block[index]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope
								|| block[index]->assignment.expressions.back()->type != AST_EXPRESSION_CONSTANT
								|| !get_constant_type(block[index]->assignment.expressions.back()))
								index = INVALID_ID;
							break;
						}

						if (index == INVALID_ID) break;
						hasBoolConstruct = false;

						if (i >= 3
							&& block[i]->type == AST_STATEMENT_ASSIGNMENT
							&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
							&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_TRUE
							&& (block[i - 1]->type == AST_STATEMENT_GOTO
								|| block[i - 1]->type == AST_STATEMENT_BREAK)
							&& block[i - 1]->instruction.type == Bytecode::BC_OP_JMP
							&& block[i - 1]->instruction.target == function.labels[targetLabel].target
							&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
							&& block[i - 2]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
							&& block[i - 2]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE
							&& block[i - 2]->assignment.variables.size() == 1
							&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
							&& *block[i - 2]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope) {
							switch (block[i - 3]->type) {
							case AST_STATEMENT_CONDITION:
								if (block[i - 3]->assignment.expressions.size() == 2 && block[i - 3]->instruction.target == block[i]->instruction.id) hasBoolConstruct = true;
								break;
							case AST_STATEMENT_GOTO:
							case AST_STATEMENT_BREAK:
								if (i < 5
									|| block[i - 3]->instruction.type != Bytecode::BC_OP_JMP
									|| block[i - 3]->instruction.target != function.labels[extendedTargetLabel].target
									|| (!function.is_valid_label(block[i]->instruction.attachedLabel)
										&& !function.is_valid_label(block[i - 2]->instruction.attachedLabel))
									|| block[i - 4]->type != AST_STATEMENT_ASSIGNMENT
									|| block[i - 4]->assignment.variables.size() != 1
									|| block[i - 4]->assignment.variables.back().type != AST_VARIABLE_SLOT
									|| block[i - 4]->assignment.variables.back().slot != block[i]->assignment.variables.back().slot)
									break;

								if (index == i - 2 && !function.is_valid_label(block[i]->instruction.attachedLabel)) {
									if (function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.front() > block[i - 2]->instruction.id) break;
									index = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.front() - 1);

									if (index == INVALID_ID) {
										index = i - 2;
										break;
									}
								}

								hasBoolConstruct = true;
								break;
							}

							if (hasBoolConstruct) {
								if ((function.is_valid_label(block[i]->instruction.attachedLabel)
									&& function.labels[block[i]->instruction.attachedLabel].jumpIds.back() >= block[i]->instruction.id)
									|| (function.is_valid_label(block[i - 2]->instruction.attachedLabel)
										&& function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.back() >= block[i - 2]->instruction.id))
									break;

								if (function.is_valid_label(block[i]->instruction.attachedLabel)) {
									for (uint32_t j = function.labels[block[i]->instruction.attachedLabel].jumpIds.size(); j--;) {
										targetIndex = get_block_index_from_id(block, function.labels[block[i]->instruction.attachedLabel].jumpIds[j] - 1);

										if (targetIndex == INVALID_ID
											|| block[targetIndex]->type != AST_STATEMENT_CONDITION
											|| block[targetIndex]->assignment.variables.size()) {
											index = INVALID_ID;
											break;
										}

										if (!block[targetIndex]->assignment.expressions.size()) {
											hasBoolConstruct = false;
											break;
										}
									}
								}

								if (hasBoolConstruct && function.is_valid_label(block[i - 2]->instruction.attachedLabel)) {
									for (uint32_t j = function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.size(); j--;) {
										targetIndex = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.attachedLabel].jumpIds[j] - 1);

										if (targetIndex == INVALID_ID || block[targetIndex]->type != AST_STATEMENT_CONDITION) {
											index = INVALID_ID;
											break;
										}

										if (!block[targetIndex]->assignment.expressions.size() || block[targetIndex]->assignment.variables.size()) {
											hasBoolConstruct = false;
											break;
										}
									}
								}

								if (index == INVALID_ID) break;
							}
						}

						for (uint32_t j = i; index != INVALID_ID && block[index]->instruction.id < block[j]->instruction.id; j--) {
							if (function.is_valid_label(block[j]->instruction.attachedLabel)) {
								if (function.labels[block[j]->instruction.attachedLabel].jumpIds.back() >= block[j]->instruction.id) {
									index = INVALID_ID;
									break;
								}

								while (function.labels[block[j]->instruction.attachedLabel].jumpIds.front() < block[index]->instruction.id) {
									if (!index) {
										index = INVALID_ID;
										break;
									}

									index--;
								}
							}
						}

						if (index != INVALID_ID) {
							switch (block[index]->type) {
							case AST_STATEMENT_CONDITION:
								if (block[index]->assignment.variables.size()) break;
							case AST_STATEMENT_GOTO:
							case AST_STATEMENT_BREAK:
								if (block[index]->instruction.target == function.labels[targetLabel].target && index) index--;
							}

							ConditionBuilder conditionBuilder(ConditionBuilder::ASSIGNMENT, *this, targetLabel,
								hasBoolConstruct ? block[i]->instruction.attachedLabel : INVALID_ID, hasBoolConstruct ? block[i - 2]->instruction.attachedLabel : INVALID_ID);
							targetIndex = hasBoolConstruct ? (block[i - 3]->type == AST_STATEMENT_GOTO ? i - 4 : i - 2) : i;

							for (uint32_t j = index; j < targetIndex; j++) {
								switch (block[j]->type) {
								case AST_STATEMENT_CONDITION:
									if (block[j]->instruction.target <= block[j]->instruction.id
										|| block[j]->instruction.target > function.labels[targetLabel].target
										|| (block[j]->instruction.target == function.labels[targetLabel].target
											? !block[j]->assignment.variables.size() || *block[j]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope
											: block[j]->assignment.variables.size()))
										break;
									conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j]->instruction.attachedLabel,
										function.get_label_from_id(block[j]->instruction.target), &block[j]->assignment.expressions);
									continue;
								case AST_STATEMENT_ASSIGNMENT:
									if (block[j]->assignment.variables.size() != 1
										|| block[j]->assignment.variables.back().type != AST_VARIABLE_SLOT
										|| *block[j]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope
										|| j + 1 == targetIndex
										|| function.is_valid_label(block[j + 1]->instruction.attachedLabel))
										break;
									j++;

									switch (block[j]->type) {
									case AST_STATEMENT_CONDITION:
										if (block[j]->instruction.target != function.labels[targetLabel].target
											|| block[j]->assignment.variables.size()
											|| block[j]->assignment.expressions.size() != 1
											|| block[j]->assignment.expressions.back()->type != AST_EXPRESSION_VARIABLE
											|| block[j]->assignment.expressions.back()->variable->type != AST_VARIABLE_SLOT
											|| *block[j]->assignment.expressions.back()->variable->slotScope != *block[i]->assignment.variables.back().slotScope)
											break;
										conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j - 1]->instruction.attachedLabel,
											function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
										continue;
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										if (block[j]->instruction.type != Bytecode::BC_OP_JMP
											|| block[j]->instruction.target != function.labels[targetLabel].target
											|| block[j - 1]->assignment.expressions.back()->type != AST_EXPRESSION_CONSTANT
											|| !get_constant_type(block[j - 1]->assignment.expressions.back()))
											break;

										switch (block[j - 1]->assignment.expressions.back()->constant->type) {
										case AST_CONSTANT_NIL:
										case AST_CONSTANT_FALSE:
											conditionBuilder.add_node(ConditionBuilder::Node::FALSY_TEST, block[j - 1]->instruction.attachedLabel,
												function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
											break;
										case AST_CONSTANT_TRUE:
										case AST_CONSTANT_STRING:
										case AST_CONSTANT_NUMBER:
											conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[j - 1]->instruction.attachedLabel,
												function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
											break;
										}

										continue;
									}

									break;
								}

								index = INVALID_ID;
								break;
							}

							if (!hasBoolConstruct) {
								conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i]->instruction.attachedLabel, targetLabel, &block[i]->assignment.expressions);
							} else if (block[i - 3]->type == AST_STATEMENT_GOTO) {
								conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i - 4]->instruction.attachedLabel, targetLabel, &block[i - 4]->assignment.expressions);
							}

							if (index != INVALID_ID) {
								//TODO
								expression = conditionBuilder.build_condition();
								assert(expression, "Failed to build condition in function " + std::to_string(function.id), bytecode.filePath, DEBUG_INFO);
								block[i]->assignment.expressions.back() = expression;

								for (uint32_t j = index; j < i; j++) {
									switch (block[j]->type) {
									case AST_STATEMENT_CONDITION:
										if (block[j]->instruction.target == function.labels[targetLabel].target) (*block[i]->assignment.variables.back().slotScope)->usages--;
										function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
										if (block[j]->assignment.variables.size()) function.remove_jump(block[j]->instruction.id, block[j]->instruction.id + 2);
										continue;
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										function.remove_jump(block[j]->instruction.id, block[j]->instruction.target);
										continue;
									case AST_STATEMENT_ASSIGNMENT:
										(*block[i]->assignment.variables.back().slotScope)->usages--;
										continue;
									}
								}

								block[i]->instruction.attachedLabel = block[index]->instruction.attachedLabel;
								block.erase(block.begin() + index, block.begin() + i);
								i = index;
							}
						}
					} else {
						if ((*block[i]->assignment.variables.back().slotScope)->usages == 1
							&& (i == block.size() - 1
								|| block[i + 1]->type != AST_STATEMENT_DECLARATION))
							break;
						//TODO
					}

					break;
				case AST_VARIABLE_TABLE_INDEX:
					if (i
						&& !function.is_valid_label(block[i]->instruction.attachedLabel)
						&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
						&& block[i - 1]->assignment.variables.size() == 1
						&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.variables.back().table->variable->slot) {
						if (block[i - 1]->assignment.isTableConstructor
							&& !block[i - 1]->assignment.expressions.back()->table->multresField
							&& (block[i]->assignment.variables.back().isMultres
								|| get_constant_type(block[i]->assignment.variables.back().tableIndex) <= NIL_CONSTANT
								|| !get_constant_type(block[i]->assignment.expressions.back()))) {
							for (uint32_t j = block[i]->assignment.usedSlots.size(); j--
								&& block[i]->assignment.usedSlots[j] != block[i]->assignment.variables.back().table->variable->slot;) {
								block[i]->assignment.usedSlots.erase(block[i]->assignment.usedSlots.begin() + j);
							}

							if (!block[i]->assignment.usedSlots.size()) {
								if (block[i]->assignment.variables.back().isMultres) {
									block[i - 1]->assignment.expressions.back()->table->multresIndex = block[i]->assignment.variables.back().multresIndex;
									block[i - 1]->assignment.expressions.back()->table->multresField = block[i]->assignment.expressions.back();
								} else {
									block[i - 1]->assignment.expressions.back()->table->fields.emplace_back();
									block[i - 1]->assignment.expressions.back()->table->fields.back().key = block[i]->assignment.variables.back().tableIndex;
									block[i - 1]->assignment.expressions.back()->table->fields.back().value = block[i]->assignment.expressions.back();
								}

								(*block[i - 1]->assignment.variables.back().slotScope)->usages--;
								block.erase(block.begin() + i);
								i--;
								break;
							}
						}

						if (!block[i]->assignment.variables.back().isMultres && (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1) {
							block[i]->assignment.variables.back().table = block[i - 1]->assignment.expressions.back();
							function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
							block[i]->instruction.attachedLabel = block[i - 1]->instruction.attachedLabel;
							i--;
							block.erase(block.begin() + i);
							break;
						}
					}

					assert(!block[i]->assignment.variables.back().isMultres, "Unable to eliminate multres table index", bytecode.filePath, DEBUG_INFO);
					break;
				}
			}

			break;
		}
	}
}

void Ast::eliminate_conditions(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	std::vector<Expression*> expressions(1);
	uint32_t index, targetIndex, previousValidIndex, assignmentIndex, targetLabel, extendedTargetLabel;
	bool hasBoolConstruct, hasEndAssignment;

	for (uint32_t i = block.size(); i--;) {
		if (block[i]->instruction.id == INVALID_ID) continue;
		blockInfo.index = i;
		targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
		extendedTargetLabel = get_label_from_next_statement(function, blockInfo, true, false);
		if (!function.is_valid_label(targetLabel) || function.labels[targetLabel].jumpIds.front() > block[i]->instruction.id) continue;

		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			index = INVALID_ID;

			for (uint32_t j = function.labels[targetLabel].jumpIds.size(); j--;) {
				if (function.labels[targetLabel].jumpIds[j] > block[i]->instruction.id) continue;
				index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds[j]);
				if (index == INVALID_ID) break;

				switch (block[index]->type) {
				case AST_STATEMENT_CONDITION:
					if (!block[index]->assignment.variables.size()) {
						index = INVALID_ID;
						if (targetLabel == extendedTargetLabel
							|| (block[index]->assignment.expressions.size() == 1
								&& block[index]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
								&& block[index]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT))
							continue;
					}
						
					break;
				case AST_STATEMENT_ASSIGNMENT:
					if (block[index + 1]->instruction.type == Bytecode::BC_OP_JMP
						&& block[index]->assignment.variables.size() == 1
						&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[index]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
						&& !get_constant_type(block[index]->assignment.expressions.back()))
						break;
				default:
					index = INVALID_ID;
				}

				break;
			}

			if (index == INVALID_ID) continue;
			assignmentIndex = index;
			break;
		case AST_STATEMENT_GOTO:
		case AST_STATEMENT_BREAK:
			if (!i
				|| block[i]->instruction.type != Bytecode::BC_OP_JMP
				|| block[i]->instruction.target != function.labels[targetLabel].target
				|| block[i - 1]->type != AST_STATEMENT_ASSIGNMENT
				|| block[i - 1]->assignment.variables.size() != 1
				|| block[i - 1]->assignment.variables.back().type != AST_VARIABLE_SLOT
				|| block[i - 1]->assignment.expressions.back()->type != AST_EXPRESSION_CONSTANT
				|| !get_constant_type(block[i - 1]->assignment.expressions.back()))
				continue;
			assignmentIndex = i - 1;
			break;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() != 1 || block[i]->assignment.variables.back().type != AST_VARIABLE_SLOT) continue;
			assignmentIndex = i;
			break;
		default:
			continue;
		}

		index = assignmentIndex;
		hasBoolConstruct = false;

		if (i >= 3
			&& block[i]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
			&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_TRUE
			&& (block[i - 1]->type == AST_STATEMENT_GOTO
				|| block[i - 1]->type == AST_STATEMENT_BREAK)
			&& block[i - 1]->instruction.type == Bytecode::BC_OP_JMP
			&& block[i - 1]->instruction.target == function.labels[targetLabel].target
			&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i - 2]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
			&& block[i - 2]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE
			&& block[i - 2]->assignment.variables.size() == 1
			&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& block[i - 2]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot) {
			switch (block[i - 3]->type) {
			case AST_STATEMENT_CONDITION:
				if (block[i - 3]->assignment.expressions.size() == 2 && block[i - 3]->instruction.target == block[i]->instruction.id) hasBoolConstruct = true;
				break;
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				if (i < 4
					|| block[i - 3]->instruction.type != Bytecode::BC_OP_JMP
					|| block[i - 3]->instruction.target != function.labels[extendedTargetLabel].target
					|| (!function.is_valid_label(block[i]->instruction.attachedLabel)
						&& !function.is_valid_label(block[i - 2]->instruction.attachedLabel)))
					break;

				if (index == i - 2 && !function.is_valid_label(block[i]->instruction.attachedLabel)) {
					if (function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.front() < block[i - 2]->instruction.id) break;
					index = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.front() - 1);
					
					if (index == INVALID_ID) {
						index = i - 2;
						break;
					}
				}

				hasBoolConstruct = true;
				break;
			}

			if (hasBoolConstruct) {
				if ((function.is_valid_label(block[i]->instruction.attachedLabel)
					&& function.labels[block[i]->instruction.attachedLabel].jumpIds.back() >= block[i]->instruction.id)
					|| (function.is_valid_label(block[i - 2]->instruction.attachedLabel)
						&& function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.back() >= block[i - 2]->instruction.id))
					continue;

				if (function.is_valid_label(block[i]->instruction.attachedLabel)) {
					for (uint32_t j = function.labels[block[i]->instruction.attachedLabel].jumpIds.size(); j--;) {
						targetIndex = get_block_index_from_id(block, function.labels[block[i]->instruction.attachedLabel].jumpIds[j] - 1);

						if (targetIndex == INVALID_ID
							|| block[targetIndex]->type != AST_STATEMENT_CONDITION
							|| block[targetIndex]->assignment.variables.size()) {
							index = INVALID_ID;
							break;
						}

						if (!block[targetIndex]->assignment.expressions.size()) {
							hasBoolConstruct = false;
							break;
						}
					}
				}

				if (hasBoolConstruct && function.is_valid_label(block[i - 2]->instruction.attachedLabel)) {
					for (uint32_t j = function.labels[block[i - 2]->instruction.attachedLabel].jumpIds.size(); j--;) {
						targetIndex = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.attachedLabel].jumpIds[j] - 1);

						if (targetIndex == INVALID_ID || block[targetIndex]->type != AST_STATEMENT_CONDITION) {
							index = INVALID_ID;
							break;
						}

						if (!block[targetIndex]->assignment.expressions.size() || block[targetIndex]->assignment.variables.size()) {
							hasBoolConstruct = false;
							break;
						}
					}
				}

				if (index == INVALID_ID) continue;
			}
		}

		previousValidIndex = INVALID_ID;
		hasEndAssignment = hasBoolConstruct ? block[i - 3]->type == AST_STATEMENT_CONDITION || block[i - 4]->type == AST_STATEMENT_ASSIGNMENT : block[i]->type == AST_STATEMENT_ASSIGNMENT;
		targetIndex = hasBoolConstruct ? (block[i - 3]->type == AST_STATEMENT_GOTO ? i - (hasEndAssignment ? 4 : 3) : i - 2) : (hasEndAssignment ? i : i + 1);

		for (uint32_t j = function.labels[targetLabel].jumpIds.size(); j--;) {
			if (function.labels[targetLabel].jumpIds[j] > block[i]->instruction.id
				|| (j
					&& function.labels[targetLabel].jumpIds[j - 1] < block[index]->instruction.id))
				continue;

			if (function.labels[targetLabel].jumpIds[j] < block[index]->instruction.id) {
				index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds[j] - 1);
			}

			for (uint32_t k = i; index != INVALID_ID && block[index]->instruction.id < block[k]->instruction.id; k--) {
				if (function.is_valid_label(block[k]->instruction.attachedLabel)) {
					if (function.labels[block[k]->instruction.attachedLabel].jumpIds.back() >= block[k]->instruction.id) {
						index = INVALID_ID;
						break;
					}

					while (function.labels[block[k]->instruction.attachedLabel].jumpIds.front() < block[index]->instruction.id) {
						if (!index) {
							index = INVALID_ID;
							break;
						}

						index--;
					}
				}
			}

			if (index == INVALID_ID) break;

			switch (block[index]->type) {
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				if (block[index]->instruction.target == function.labels[targetLabel].target && index) index--;
			}

			for (uint32_t k = index; k < targetIndex; k++) {
				switch (block[k]->type) {
				case AST_STATEMENT_CONDITION:
					if (block[k]->assignment.variables.size()) {
						if (block[k]->instruction.target == function.labels[targetLabel].target
							&& block[k]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot)
							continue;
					} else if (block[k]->instruction.target == function.labels[targetLabel].target
						&& block[k]->assignment.expressions.size() == 1
						&& block[k]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
						&& block[k]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
						&& block[k]->assignment.expressions.back()->variable->slot == block[assignmentIndex]->assignment.variables.back().slot) {
						continue;
					} else if (block[k]->instruction.target == function.labels[extendedTargetLabel].target && !hasEndAssignment) {
						continue;
					} else if (block[k]->instruction.target > block[k]->instruction.id && block[k]->instruction.target < function.labels[targetLabel].target) {
						continue;
					}

					break;
				case AST_STATEMENT_ASSIGNMENT:
					if (block[k]->assignment.variables.size() == 1
						&& block[k]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[k]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot
						&& block[k]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
						&& get_constant_type(block[k]->assignment.expressions.back())
						&& ++k != targetIndex
						&& (block[k]->type == AST_STATEMENT_GOTO
							|| block[k]->type == AST_STATEMENT_BREAK)
						&& block[k]->instruction.type == Bytecode::BC_OP_JMP
						&& block[k]->instruction.target == function.labels[targetLabel].target)
						continue;
					break;
				}

				index = INVALID_ID;
				break;
			}

			if (index == INVALID_ID) break;
			previousValidIndex = index;
		}

		if (previousValidIndex == INVALID_ID) continue;

		ConditionBuilder conditionBuilder(ConditionBuilder::ASSIGNMENT, *this, targetLabel,
			hasBoolConstruct ? block[i]->instruction.attachedLabel : INVALID_ID, hasBoolConstruct ? block[i - 2]->instruction.attachedLabel : INVALID_ID);

		for (uint32_t j = previousValidIndex; j < targetIndex; j++) {
			switch (block[j]->type) {
			case AST_STATEMENT_CONDITION:
				conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j]->instruction.attachedLabel,
					hasEndAssignment
					|| block[j]->assignment.variables.size()
					|| (block[j]->instruction.target == function.labels[targetLabel].target ? targetLabel != extendedTargetLabel : block[j]->instruction.target != function.labels[extendedTargetLabel].target)
					? function.get_label_from_id(block[j]->instruction.target) : function.labels.size(), &block[j]->assignment.expressions);
				continue;
			case AST_STATEMENT_ASSIGNMENT:
				switch (block[j]->assignment.expressions.back()->constant->type) {
				case AST_CONSTANT_NIL:
				case AST_CONSTANT_FALSE:
					conditionBuilder.add_node(ConditionBuilder::Node::FALSY_TEST, block[j]->instruction.attachedLabel,
						function.get_label_from_id(block[j + 1]->instruction.target), &block[j]->assignment.expressions);
					break;
				case AST_CONSTANT_TRUE:
				case AST_CONSTANT_STRING:
				case AST_CONSTANT_NUMBER:
					conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[j]->instruction.attachedLabel,
						function.get_label_from_id(block[j + 1]->instruction.target), &block[j]->assignment.expressions);
					break;
				}

				j++;
				continue;
			}
		}

		if (hasEndAssignment) {
			if (!hasBoolConstruct) {
				conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i]->instruction.attachedLabel, targetLabel, &block[i]->assignment.expressions);
			} else if (block[i - 3]->type == AST_STATEMENT_GOTO) {
				conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i - 4]->instruction.attachedLabel, targetLabel, &block[i - 4]->assignment.expressions);
			}
		} else {
			expressions.back() = new_slot(block[assignmentIndex]->assignment.variables.back().slot);
			expressions.back()->variable->slotScope = block[assignmentIndex]->assignment.variables.back().slotScope;
			conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, function.labels.size(), targetLabel, &expressions);
		}
		
		//TODO
		expressions.back() = conditionBuilder.build_condition();
		assert(expressions.back(), "Failed to build condition in function " + std::to_string(function.id), bytecode.filePath, DEBUG_INFO);
		block[assignmentIndex]->assignment.expressions.back() = expressions.back();

		for (uint32_t j = index; j < i; j++) {
			switch (block[j]->type) {
			case AST_STATEMENT_CONDITION:
				function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
				if (!block[j]->assignment.variables.size()) continue;
				function.remove_jump(block[j]->instruction.id, block[j]->instruction.id + 2);
			case AST_STATEMENT_ASSIGNMENT:
				if (*block[j]->assignment.variables.back().slotScope != *block[assignmentIndex]->assignment.variables.back().slotScope) {
					(*block[assignmentIndex]->assignment.variables.back().slotScope)->usages += (*block[j]->assignment.variables.back().slotScope)->usages;
					if ((*block[j]->assignment.variables.back().slotScope)->scopeBegin < (*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeBegin)
						(*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeBegin = (*block[j]->assignment.variables.back().slotScope)->scopeBegin;
					if ((*block[j]->assignment.variables.back().slotScope)->scopeEnd > (*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeEnd)
						(*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeEnd = (*block[j]->assignment.variables.back().slotScope)->scopeEnd;
					*block[j]->assignment.variables.back().slotScope = *block[assignmentIndex]->assignment.variables.back().slotScope;
					if (block[j]->assignment.variables.back().slotScope != block[assignmentIndex]->assignment.variables.back().slotScope)
						function.slotScopeCollector.remove_scope(block[j]->assignment.variables.back().slot, block[j]->assignment.variables.back().slotScope);
				}

				continue;
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				function.remove_jump(block[j]->instruction.id, block[j]->instruction.target);
				continue;
			}
		}

		block[i] = block[assignmentIndex];
		block[i]->type = AST_STATEMENT_ASSIGNMENT;
		block[i]->instruction.attachedLabel = block[index]->instruction.attachedLabel;
		if ((*block[i]->assignment.variables.back().slotScope)->scopeBegin >= block[index]->instruction.id) block[i]->assignment.needsFordwardDeclaration = true;
		block.erase(block.begin() + index, block.begin() + i);
		i = index;
	}

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			blockInfo.index = i;
			targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
			targetIndex = INVALID_ID;
			index = i;

			while (index && block[index - 1]->type == AST_STATEMENT_CONDITION) {
				index--;
			}

			for (uint32_t j = index; j <= i; j++) {
				if (function.is_valid_label(block[j]->instruction.attachedLabel)) {
					if (function.labels[block[j]->instruction.attachedLabel].jumpIds.front() < block[index]->instruction.id
						|| function.labels[block[j]->instruction.attachedLabel].jumpIds.back() > block[j]->instruction.id) {
						index = j;
						targetIndex = INVALID_ID;
					} else if ((j
						&& j - 1 >= index
						&& block[j - 1]->instruction.target == function.labels[block[j]->instruction.attachedLabel].target)) {
						for (uint32_t k = index; k < j
							&& block[k]->instruction.target > block[k]->instruction.id
							&& block[k]->instruction.target <= block[j]->instruction.id; k++) {
							if (k != j - 1) continue;
							index = j;
							targetIndex = INVALID_ID;
							break;
						}
					}
				}

				if ((targetLabel == INVALID_ID
					|| block[j]->instruction.target != function.labels[targetLabel].target)
					&& (block[j]->instruction.target < block[j]->instruction.id
						|| block[j]->instruction.target > block[i]->instruction.id)) {
					if (targetIndex != INVALID_ID) {
						if (block[j]->instruction.target == block[targetIndex]->instruction.target) continue;
						index = targetIndex + 1;
						j = targetIndex;
						targetIndex = INVALID_ID;
						continue;
					}

					targetIndex = j;
				}
			}

			if (targetIndex == INVALID_ID) {
				extendedTargetLabel = targetLabel;
				targetLabel = INVALID_ID;
			} else {
				extendedTargetLabel = function.get_label_from_id(block[targetIndex]->instruction.target);
			}

			{
				ConditionBuilder conditionBuilder(ConditionBuilder::STATEMENT, *this, INVALID_ID, targetLabel, extendedTargetLabel);

				for (uint32_t j = index; j <= i; j++) {
					assert(!block[j]->assignment.variables.size(), "Failed to eliminate all test and copy conditions", bytecode.filePath, DEBUG_INFO);
					conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped),
						block[j]->instruction.attachedLabel, function.get_label_from_id(block[j]->instruction.target), &block[j]->assignment.expressions);
				}

				expressions.back() = conditionBuilder.build_condition();
				assert(expressions.back(), "Failed to build condition in function " + std::to_string(function.id), bytecode.filePath, DEBUG_INFO);
				block[i]->assignment.expressions = expressions;

				for (uint32_t j = index; j <= i; j++) {
					function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
				}

				block[i]->instruction.target = function.labels[extendedTargetLabel].target;
				function.add_jump(block[i]->instruction.id, block[i]->instruction.target);
				block[i]->instruction.attachedLabel = block[index]->instruction.attachedLabel;
				block.erase(block.begin() + index, block.begin() + i);
				i = index;
			}

			continue;
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			eliminate_conditions(function, block[i]->block, &blockInfo);
			continue;
		}
	}
}

void Ast::build_if_statements(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	uint32_t index, targetLabel;

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			//TODO
			block[i]->type = AST_STATEMENT_IF;
			targetLabel = INVALID_ID;

			for (index = i; index < block.size(); index++) {
				blockInfo.index = index;
				targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
				if (targetLabel != INVALID_ID && function.labels[targetLabel].target == block[i]->instruction.target) break;
			}

			assert(targetLabel != INVALID_ID && function.labels[targetLabel].target == block[i]->instruction.target, "Failed to build if statement", bytecode.filePath, DEBUG_INFO);
			block[i]->block.reserve(index - i);
			block[i]->block.insert(block[i]->block.begin(), block.begin() + i + 1, block.begin() + index + 1);
			block.erase(block.begin() + i + 1, block.begin() + index + 1);
		case AST_STATEMENT_BREAK:
			function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);
			continue;
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			build_if_statements(function, block[i]->block, &blockInfo);
			continue;
		}
	}
}

uint32_t Ast::get_block_index_from_id(const std::vector<Statement*>& block, const uint32_t& id) {
	for (uint32_t i = block.size(); i-- && (block[i]->instruction.id == INVALID_ID || block[i]->instruction.id >= id);) {
		if (block[i]->instruction.id == id) return i;
	}

	return INVALID_ID;
}

uint32_t Ast::get_extended_id_from_statement(Statement* const& statement) {
	switch (statement->type) {
	case AST_STATEMENT_GOTO:
	case AST_STATEMENT_BREAK:
		if (statement->instruction.type == Bytecode::BC_OP_JMP) return statement->instruction.target;
	}

	return statement->instruction.id;
}

uint32_t Ast::get_label_from_next_statement(Function& function, const BlockInfo& blockInfo, const bool& returnExtendedLabel, const bool& excludeDeclaration) {
	if (blockInfo.index == blockInfo.block.size() - 1) {
		return blockInfo.previousBlock ? get_label_from_next_statement(function, *blockInfo.previousBlock, returnExtendedLabel, false) : INVALID_ID;
	}

	Statement* statement = blockInfo.block[blockInfo.index + 1];

	if (excludeDeclaration && statement->type == AST_STATEMENT_DECLARATION) {
		if (statement->block.size()) {
			statement = statement->block.front();
		} else if (blockInfo.index + 2 != blockInfo.block.size()) {
			statement = blockInfo.block[blockInfo.index + 2];
		} else {
			return blockInfo.previousBlock ? get_label_from_next_statement(function, *blockInfo.previousBlock, returnExtendedLabel, false) : INVALID_ID;
		}
	}

	switch (statement->type) {
	case AST_STATEMENT_GOTO:
	case AST_STATEMENT_BREAK:
		if (returnExtendedLabel && statement->instruction.type == Bytecode::BC_OP_JMP) return function.get_label_from_id(statement->instruction.target);
	}

	return statement->instruction.attachedLabel;
}

void Ast::check_valid_name(Constant* const& constant) {
	static constexpr uint8_t KEYWORDS_COUNT = 21;

	static const std::string KEYWORDS[KEYWORDS_COUNT] = {
		"and", "break", "do", "else", "elseif", "end", "false",
		"for", "function", "if", "in", "local", "nil", "not",
		"or", "repeat", "return", "then", "true", "until", "while"
	};

	if (!constant->string.size() || constant->string.front() < 'A') return;

	for (uint32_t i = constant->string.size(); i--;) {
		if (constant->string[i] < '0' || constant->string[i] > 'z') return;

		switch (constant->string[i]) {
		case ':':
		case ';':
		case '<':
		case '=':
		case '>':
		case '?':
		case '@':
		case '[':
		case '\\':
		case ']':
		case '^':
		case '`':
			return;
		}
	}

	for (uint8_t i = KEYWORDS_COUNT; i--;) {
		if (KEYWORDS[i] == constant->string) return;
	}

	constant->isName = true;
}

void Ast::check_special_number(Expression* const& expression, const bool& isCdata) {
	const uint64_t rawDouble = std::bit_cast<uint64_t>(expression->constant->number);
	if ((rawDouble & DOUBLE_EXPONENT) != DOUBLE_SPECIAL) return assert(rawDouble != DOUBLE_NEGATIVE_ZERO || isCdata, "Number constant is negative zero", bytecode.filePath, DEBUG_INFO);
	assert(!(rawDouble & DOUBLE_FRACTION), "Number constant is NaN", bytecode.filePath, DEBUG_INFO);
	if (isCdata) return;
	expression->set_type(AST_EXPRESSION_BINARY_OPERATION);
	expression->binaryOperation->type = AST_BINARY_DIVISION;
	expression->binaryOperation->leftOperand = new_expression(AST_EXPRESSION_CONSTANT);
	expression->binaryOperation->leftOperand->constant->type = AST_CONSTANT_NUMBER;
	expression->binaryOperation->leftOperand->constant->number = rawDouble & DOUBLE_SIGN ? -1 : 1;
	expression->binaryOperation->rightOperand = new_expression(AST_EXPRESSION_CONSTANT);
	expression->binaryOperation->rightOperand->constant->type = AST_CONSTANT_NUMBER;
	expression->binaryOperation->rightOperand->constant->number = 0;
}

Ast::CONSTANT_TYPE Ast::get_constant_type(Expression* const& expression) {
	static bool (* const is_valid_number_constant)(const double& number) = [](const double& number)->bool {
		const uint64_t rawDouble = std::bit_cast<uint64_t>(number);
		return (rawDouble & DOUBLE_EXPONENT) == DOUBLE_SPECIAL ? !(rawDouble & DOUBLE_FRACTION) : rawDouble != DOUBLE_NEGATIVE_ZERO;
	};

	switch (expression->type) {
	case AST_EXPRESSION_CONSTANT:
		switch (expression->constant->type) {
		case AST_CONSTANT_NIL:
			return NIL_CONSTANT;
		case AST_CONSTANT_FALSE:
		case AST_CONSTANT_TRUE:
		case AST_CONSTANT_STRING:
			return BOOL_CONSTANT;
		case AST_CONSTANT_NUMBER:
			return NUMBER_CONSTANT;
		}

		break;
	case AST_EXPRESSION_BINARY_OPERATION:
		switch (expression->binaryOperation->type) {
		case AST_BINARY_ADDITION:
		case AST_BINARY_SUBTRACTION:
		case AST_BINARY_MULTIPLICATION:
		case AST_BINARY_DIVISION:
		case AST_BINARY_EXPONENTATION:
		case AST_BINARY_MODULO:
			if (get_constant_type(expression->binaryOperation->leftOperand) != NUMBER_CONSTANT || get_constant_type(expression->binaryOperation->rightOperand) != NUMBER_CONSTANT) break;
			double number;

			switch (expression->binaryOperation->type) {
			case AST_BINARY_ADDITION:
				number = expression->binaryOperation->leftOperand->constant->number + expression->binaryOperation->rightOperand->constant->number;
				break;
			case AST_BINARY_SUBTRACTION:
				number = expression->binaryOperation->leftOperand->constant->number - expression->binaryOperation->rightOperand->constant->number;
				break;
			case AST_BINARY_MULTIPLICATION:
				number = expression->binaryOperation->leftOperand->constant->number * expression->binaryOperation->rightOperand->constant->number;
				break;
			case AST_BINARY_DIVISION:
				number = expression->binaryOperation->leftOperand->constant->number / expression->binaryOperation->rightOperand->constant->number;
				break;
			case AST_BINARY_EXPONENTATION:
				number = std::pow(expression->binaryOperation->leftOperand->constant->number, expression->binaryOperation->rightOperand->constant->number);
				break;
			case AST_BINARY_MODULO:
				number = std::fmod(expression->binaryOperation->leftOperand->constant->number, expression->binaryOperation->rightOperand->constant->number);
				break;
			}

			if (is_valid_number_constant(number)) return NUMBER_CONSTANT;
		}

		break;
	case AST_EXPRESSION_UNARY_OPERATION:
		switch (expression->unaryOperation->type) {
		case AST_UNARY_NOT:
			if (get_constant_type(expression->unaryOperation->operand)) return BOOL_CONSTANT;
			break;
		case AST_UNARY_MINUS:
			if (expression->unaryOperation->operand->type == AST_EXPRESSION_CONSTANT) {
				switch (expression->unaryOperation->operand->constant->type) {
				case AST_CONSTANT_NUMBER:
					if (!is_valid_number_constant(-expression->unaryOperation->operand->constant->number)) break;
				case AST_CONSTANT_CDATA_SIGNED:
				case AST_CONSTANT_CDATA_UNSIGNED:
				case AST_CONSTANT_CDATA_IMAGINARY:
					return NUMBER_CONSTANT;
				}
			}

			break;
		}

		break;
	}

	return INVALID_CONSTANT;
}

Ast::Expression* Ast::new_slot(const uint8_t& slot) {
	Expression* const expression = new_expression(AST_EXPRESSION_VARIABLE);
	expression->variable->type = AST_VARIABLE_SLOT;
	expression->variable->slot = slot;
	return expression;
}

Ast::Expression* Ast::new_literal(const uint8_t& literal) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);
	expression->constant->type = AST_CONSTANT_NUMBER;
	expression->constant->number = literal;
	return expression;
}

Ast::Expression* Ast::new_signed_literal(const uint16_t& signedLiteral) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);
	expression->constant->type = AST_CONSTANT_NUMBER;
	expression->constant->number = std::bit_cast<int16_t>(signedLiteral);
	return expression;
}

Ast::Expression* Ast::new_primitive(const uint8_t& primitive) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);

	switch (primitive) {
	case 0:
		expression->constant->type = AST_CONSTANT_NIL;
		break;
	case 1:
		expression->constant->type = AST_CONSTANT_FALSE;
		break;
	case 2:
		expression->constant->type = AST_CONSTANT_TRUE;
		break;
	}

	return expression;
}

Ast::Expression* Ast::new_number(const Function& function, const uint16_t& index) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);
	expression->constant->type = AST_CONSTANT_NUMBER;

	switch (function.get_number_constant(index).type) {
	case Bytecode::BC_KNUM_INT:
		expression->constant->number = std::bit_cast<int32_t>(function.get_number_constant(index).integer);
		break;
	case Bytecode::BC_KNUM_NUM:
		expression->constant->number = std::bit_cast<double>(function.get_number_constant(index).number);
		check_special_number(expression);
		break;
	}

	return expression;
}

Ast::Expression* Ast::new_string(const Function& function, const uint16_t& index) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);
	expression->constant->type = AST_CONSTANT_STRING;
	expression->constant->string = function.get_constant(index).string;
	return expression;
}

Ast::Expression* Ast::new_table(const Function& function, const uint16_t& index) {
	static Expression* (* const new_table_constant)(Ast& ast, const Bytecode::TableConstant& constant) = [](Ast& ast, const Bytecode::TableConstant& constant)->Expression* {
		Expression* const expression = ast.new_expression(AST_EXPRESSION_CONSTANT);

		switch (constant.type) {
		case Bytecode::BC_KTAB_NIL:
			expression->constant->type = AST_CONSTANT_NIL;
			break;
		case Bytecode::BC_KTAB_FALSE:
			expression->constant->type = AST_CONSTANT_FALSE;
			break;
		case Bytecode::BC_KTAB_TRUE:
			expression->constant->type = AST_CONSTANT_TRUE;
			break;
		case Bytecode::BC_KTAB_INT:
			expression->constant->type = AST_CONSTANT_NUMBER;
			expression->constant->number = std::bit_cast<int32_t>(constant.integer);
			break;
		case Bytecode::BC_KTAB_NUM:
			expression->constant->type = AST_CONSTANT_NUMBER;
			expression->constant->number = std::bit_cast<double>(constant.number);
			ast.check_special_number(expression);
			break;
		case Bytecode::BC_KTAB_STR:
			expression->constant->type = AST_CONSTANT_STRING;
			expression->constant->string = constant.string;
			break;
		}

		return expression;
	};

	Expression* const expression = new_expression(AST_EXPRESSION_TABLE);
	expression->table->constants.list.resize(function.get_constant(index).array.size(), nullptr);

	for (uint32_t i = expression->table->constants.list.size(); i--;) {
		expression->table->constants.list[i] = new_table_constant(*this, function.get_constant(index).array[i]);
	}

	expression->table->constants.fields.resize(function.get_constant(index).table.size());

	for (uint32_t i = expression->table->constants.fields.size(); i--;) {
		expression->table->constants.fields[i].key = new_table_constant(*this, function.get_constant(index).table[i].key);
		if (expression->table->constants.fields[i].key->constant->type == AST_CONSTANT_STRING) check_valid_name(expression->table->constants.fields[i].key->constant);
		expression->table->constants.fields[i].value = new_table_constant(*this, function.get_constant(index).table[i].value);
	}

	return expression;
}

Ast::Expression* Ast::new_cdata(const Function& function, const uint16_t& index) {
	Expression* const expression = new_expression(AST_EXPRESSION_CONSTANT);

	switch (function.get_constant(index).type) {
	case Bytecode::BC_KGC_I64:
		expression->constant->type = AST_CONSTANT_CDATA_SIGNED;
		expression->constant->signed_integer = std::bit_cast<int64_t>(function.get_constant(index).cdata);
		break;
	case Bytecode::BC_KGC_U64:
		expression->constant->type = AST_CONSTANT_CDATA_UNSIGNED;
		expression->constant->unsigned_integer = function.get_constant(index).cdata;
		break;
	case Bytecode::BC_KGC_COMPLEX:
		expression->constant->type = AST_CONSTANT_CDATA_IMAGINARY;
		expression->constant->number = std::bit_cast<double>(function.get_constant(index).cdata);
		check_special_number(expression, true);
		break;
	}

	return expression;
}