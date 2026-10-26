#pragma once

// `{A}`, `{A:his}`, `{2.B}`: one implementation, shared.
//
// The narrator, the dialogue book and the month summariser all have to turn a
// rule's variable name into the name of the person it matched, and all three
// have to pick a gendered word the same way. This is that, lifted out of the
// narrator so there is one answer.
//
// A spec inside the braces is `[part.]variable[:form]`:
//
//   {A}       the name of the node bound to the rule's A
//   {A:his}   a word chosen by that node's gender
//   {A:His}   the same, capitalised
//   {2.A}     the SECOND part's A, for a chain or an exchange
//
// A spec that names nothing is left in the text as it was written, braces and
// all: a template with a typo shows the typo rather than silently vanishing.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "graphrewrite/runtime/RewriteEvent.hpp"

#include <functional>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct FiredEvent;

// What a placeholder needs to resolve: which programme the rules come from,
// where names live, and how to ask a node's gender.
struct PlaceholderContext
{
  const compiler::CompiledProgram *program = nullptr;
  NameTable *names = nullptr;
  std::function<Gender(uint32_t id)> genderOf;
};

// The gendered word for a form, lowercase. Unknown gender takes the neutral
// column, so a node with no gender edge still reads as a person.
std::string genderWord(const std::string &form, Gender gender);

std::string capitaliseWord(std::string word);

// The node a rule's variable is bound to in one firing, or ~0u.
uint32_t boundNode(const FiredEvent &event, const std::string &variable, const compiler::CompiledProgram &program);

// Substitutes every placeholder in `text`. `parts` is the firings a `{n.X}`
// prefix indexes; with one entry, `{X}` and `{1.X}` mean the same thing.
std::string renderPlaceholders(const std::string &text, const std::vector<const FiredEvent *> &parts, const PlaceholderContext &context);

} // namespace graphrewrite::runtime
