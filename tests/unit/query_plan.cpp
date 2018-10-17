#include "query_plan_checker.hpp"

#include <iostream>
#include <list>
#include <sstream>
#include <tuple>
#include <typeinfo>
#include <unordered_set>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "query/frontend/ast/ast.hpp"
#include "query/frontend/semantic/symbol_generator.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/plan/operator.hpp"
#include "query/plan/planner.hpp"

#include <capnp/message.h>

#include "query_common.hpp"

namespace query {
::std::ostream &operator<<(::std::ostream &os, const Symbol &sym) {
  return os << "Symbol{\"" << sym.name() << "\" [" << sym.position() << "] "
            << Symbol::TypeToString(sym.type()) << "}";
}
}  // namespace query

using namespace query::plan;
using query::AstStorage;
using query::SingleQuery;
using query::Symbol;
using query::SymbolGenerator;
using query::SymbolTable;
using Direction = query::EdgeAtom::Direction;
using Bound = ScanAllByLabelPropertyRange::Bound;

namespace {

class Planner {
 public:
  template <class TDbAccessor>
  Planner(std::vector<SingleQueryPart> single_query_parts,
          PlanningContext<TDbAccessor> &context) {
    plan_ = MakeLogicalPlanForSingleQuery<RuleBasedPlanner>(single_query_parts,
                                                            context);
  }

  auto &plan() { return *plan_; }

 private:
  std::unique_ptr<LogicalOperator> plan_;
};

template <class... TChecker>
auto CheckPlan(LogicalOperator &plan, const SymbolTable &symbol_table,
               TChecker... checker) {
  std::list<BaseOpChecker *> checkers{&checker...};
  PlanChecker plan_checker(checkers, symbol_table);
  plan.Accept(plan_checker);
  EXPECT_TRUE(plan_checker.checkers_.empty());
}

template <class TPlanner, class... TChecker>
auto CheckPlan(query::Query *query, AstStorage &storage, TChecker... checker) {
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TPlanner>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, checker...);
}

template <class T>
class TestPlanner : public ::testing::Test {};

using PlannerTypes = ::testing::Types<Planner>;

TYPED_TEST_CASE(TestPlanner, PlannerTypes);

TYPED_TEST(TestPlanner, MatchNodeReturn) {
  // Test MATCH (n) RETURN n
  AstStorage storage;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), RETURN(as_n)));
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateNodeReturn) {
  // Test CREATE (n) RETURN n AS n
  AstStorage storage;
  auto ident_n = IDENT("n");
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), RETURN(ident_n, AS("n"))));
  auto symbol_table = MakeSymbolTable(*query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc,
            ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateExpand) {
  // Test CREATE (n) -[r :rel1]-> (m)
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("relationship");
  auto *query = QUERY(SINGLE_QUERY(CREATE(PATTERN(
      NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")))));
  CheckPlan<TypeParam>(query, storage, ExpectCreateNode(),
                       ExpectCreateExpand());
}

TYPED_TEST(TestPlanner, CreateMultipleNode) {
  // Test CREATE (n), (m)
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n")), PATTERN(NODE("m")))));
  CheckPlan<TypeParam>(query, storage, ExpectCreateNode(), ExpectCreateNode());
}

TYPED_TEST(TestPlanner, CreateNodeExpandNode) {
  // Test CREATE (n) -[r :rel]-> (m), (l)
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("rel");
  auto *query = QUERY(SINGLE_QUERY(CREATE(
      PATTERN(NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")),
      PATTERN(NODE("l")))));
  CheckPlan<TypeParam>(query, storage, ExpectCreateNode(), ExpectCreateExpand(),
                       ExpectCreateNode());
}

TYPED_TEST(TestPlanner, CreateNamedPattern) {
  // Test CREATE p = (n) -[r :rel]-> (m)
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("rel");
  auto *query = QUERY(SINGLE_QUERY(CREATE(NAMED_PATTERN(
      "p", NODE("n"), EDGE("r", Direction::OUT, {relationship}), NODE("m")))));
  CheckPlan<TypeParam>(query, storage, ExpectCreateNode(), ExpectCreateExpand(),
                       ExpectConstructNamedPath());
}

TYPED_TEST(TestPlanner, MatchCreateExpand) {
  // Test MATCH (n) CREATE (n) -[r :rel1]-> (m)
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("relationship");
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {relationship}),
                     NODE("m")))));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectCreateExpand());
}

TYPED_TEST(TestPlanner, MatchLabeledNodes) {
  // Test MATCH (n :label) RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label))), RETURN(as_n)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchPathReturn) {
  // Test MATCH (n) -[r :relationship]- (m) RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("relationship");
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r", Direction::BOTH, {relationship}),
                    NODE("m"))),
      RETURN(as_n)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchNamedPatternReturn) {
  // Test MATCH p = (n) -[r :relationship]- (m) RETURN p
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("relationship");
  auto *as_p = NEXPR("p", IDENT("p"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(NAMED_PATTERN("p", NODE("n"),
                          EDGE("r", Direction::BOTH, {relationship}),
                          NODE("m"))),
      RETURN(as_p)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectConstructNamedPath(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchNamedPatternWithPredicateReturn) {
  // Test MATCH p = (n) -[r :relationship]- (m) WHERE 2 = p RETURN p
  AstStorage storage;
  FakeDbAccessor dba;
  auto relationship = dba.EdgeType("relationship");
  auto *as_p = NEXPR("p", IDENT("p"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(NAMED_PATTERN("p", NODE("n"),
                          EDGE("r", Direction::BOTH, {relationship}),
                          NODE("m"))),
      WHERE(EQ(LITERAL(2), IDENT("p"))), RETURN(as_p)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectConstructNamedPath(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, OptionalMatchNamedPatternReturn) {
  // Test OPTIONAL MATCH p = (n) -[r]- (m) RETURN p
  AstStorage storage;
  auto node_n = NODE("n");
  auto edge = EDGE("r");
  auto node_m = NODE("m");
  auto pattern = NAMED_PATTERN("p", node_n, edge, node_m);
  auto as_p = AS("p");
  auto *query = QUERY(SINGLE_QUERY(OPTIONAL_MATCH(pattern), RETURN("p", as_p)));
  auto symbol_table = MakeSymbolTable(*query);
  auto get_symbol = [&symbol_table](const auto *ast_node) {
    return symbol_table.at(*ast_node->identifier_);
  };
  std::vector<Symbol> optional_symbols{get_symbol(pattern), get_symbol(node_n),
                                       get_symbol(edge), get_symbol(node_m)};
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  std::list<BaseOpChecker *> optional{new ExpectScanAll(), new ExpectExpand(),
                                      new ExpectConstructNamedPath()};
  CheckPlan(planner.plan(), symbol_table,
            ExpectOptional(optional_symbols, optional), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWhereReturn) {
  // Test MATCH (n) WHERE n.property < 42 RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto property = dba.Property("property");
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      WHERE(LESS(PROPERTY_LOOKUP("n", property), LITERAL(42))), RETURN(as_n)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchDelete) {
  // Test MATCH (n) DELETE n
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), DELETE(IDENT("n"))));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectDelete());
}

TYPED_TEST(TestPlanner, MatchNodeSet) {
  // Test MATCH (n) SET n.prop = 42, n = n, n :label
  AstStorage storage;
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = dba.Label("label");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                                   SET(PROPERTY_LOOKUP("n", prop), LITERAL(42)),
                                   SET("n", IDENT("n")), SET("n", {label})));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectSetProperty(),
                       ExpectSetProperties(), ExpectSetLabels());
}

TYPED_TEST(TestPlanner, MatchRemove) {
  // Test MATCH (n) REMOVE n.prop REMOVE n :label
  AstStorage storage;
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto label = dba.Label("label");
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                                   REMOVE(PROPERTY_LOOKUP("n", prop)),
                                   REMOVE("n", {label})));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectRemoveProperty(),
                       ExpectRemoveLabels());
}

TYPED_TEST(TestPlanner, MatchMultiPattern) {
  // Test MATCH (n) -[r]- (m), (j) -[e]- (i) RETURN n
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m")),
                               PATTERN(NODE("j"), EDGE("e"), NODE("i"))),
                         RETURN("n")));
  // We expect the expansions after the first to have a uniqueness filter in a
  // single MATCH clause.
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpand(),
                       ExpectScanAll(), ExpectExpand(),
                       ExpectExpandUniquenessFilter<EdgeAccessor>(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchMultiPatternSameStart) {
  // Test MATCH (n), (n) -[e]- (m) RETURN n
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n")), PATTERN(NODE("n"), EDGE("e"), NODE("m"))),
      RETURN("n")));
  // We expect the second pattern to generate only an Expand, since another
  // ScanAll would be redundant.
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpand(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchMultiPatternSameExpandStart) {
  // Test MATCH (n) -[r]- (m), (m) -[e]- (l) RETURN n
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m")),
                               PATTERN(NODE("m"), EDGE("e"), NODE("l"))),
                         RETURN("n")));
  // We expect the second pattern to generate only an Expand. Another
  // ScanAll would be redundant, as it would generate the nodes obtained from
  // expansion. Additionally, a uniqueness filter is expected.
  CheckPlan<TypeParam>(
      query, storage, ExpectScanAll(), ExpectExpand(), ExpectExpand(),
      ExpectExpandUniquenessFilter<EdgeAccessor>(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiMatch) {
  // Test MATCH (n) -[r]- (m) MATCH (j) -[e]- (i) -[f]- (h) RETURN n
  AstStorage storage;
  auto *node_n = NODE("n");
  auto *edge_r = EDGE("r");
  auto *node_m = NODE("m");
  auto *node_j = NODE("j");
  auto *edge_e = EDGE("e");
  auto *node_i = NODE("i");
  auto *edge_f = EDGE("f");
  auto *node_h = NODE("h");
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(node_n, edge_r, node_m)),
      MATCH(PATTERN(node_j, edge_e, node_i, edge_f, node_h)), RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // Multiple MATCH clauses form a Cartesian product, so the uniqueness should
  // not cross MATCH boundaries.
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectScanAll(), ExpectExpand(), ExpectExpand(),
            ExpectExpandUniquenessFilter<EdgeAccessor>(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiMatchSameStart) {
  // Test MATCH (n) MATCH (n) -[r]- (m) RETURN n
  AstStorage storage;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))), RETURN(as_n)));
  // Similar to MatchMultiPatternSameStart, we expect only Expand from second
  // MATCH clause.
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithReturn) {
  // Test MATCH (old) WITH old AS new RETURN new
  AstStorage storage;
  auto *as_new = NEXPR("new", IDENT("new"));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("old"))),
                                   WITH("old", AS("new")), RETURN(as_new)));
  // No accumulation since we only do reads.
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithWhereReturn) {
  // Test MATCH (old) WITH old AS new WHERE new.prop < 42 RETURN new
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *as_new = NEXPR("new", IDENT("new"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("old"))), WITH("old", AS("new")),
      WHERE(LESS(PROPERTY_LOOKUP("new", prop), LITERAL(42))), RETURN(as_new)));
  // No accumulation since we only do reads.
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateMultiExpand) {
  // Test CREATE (n) -[r :r]-> (m), (n) - [p :p]-> (l)
  FakeDbAccessor dba;
  auto r = dba.EdgeType("r");
  auto p = dba.EdgeType("p");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      CREATE(PATTERN(NODE("n"), EDGE("r", Direction::OUT, {r}), NODE("m")),
             PATTERN(NODE("n"), EDGE("p", Direction::OUT, {p}), NODE("l")))));
  CheckPlan<TypeParam>(query, storage, ExpectCreateNode(), ExpectCreateExpand(),
                       ExpectCreateExpand());
}

TYPED_TEST(TestPlanner, MatchWithSumWhereReturn) {
  // Test MATCH (n) WITH SUM(n.prop) + 42 AS sum WHERE sum < 42
  //      RETURN sum AS result
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto sum = SUM(PROPERTY_LOOKUP("n", prop));
  auto literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))), WITH(ADD(sum, literal), AS("sum")),
      WHERE(LESS(IDENT("sum"), LITERAL(42))), RETURN("sum", AS("result"))));
  auto aggr = ExpectAggregate({sum}, {literal});
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), aggr, ExpectProduce(),
                       ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchReturnSum) {
  // Test MATCH (n) RETURN SUM(n.prop1) AS sum, n.prop2 AS group
  FakeDbAccessor dba;
  auto prop1 = dba.Property("prop1");
  auto prop2 = dba.Property("prop2");
  AstStorage storage;
  auto sum = SUM(PROPERTY_LOOKUP("n", prop1));
  auto n_prop2 = PROPERTY_LOOKUP("n", prop2);
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))), RETURN(sum, AS("sum"), n_prop2, AS("group"))));
  auto aggr = ExpectAggregate({sum}, {n_prop2});
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), aggr,
            ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateWithSum) {
  // Test CREATE (n) WITH SUM(n.prop) AS sum
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto n_prop = PROPERTY_LOOKUP("n", prop);
  auto sum = SUM(n_prop);
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))), WITH(sum, AS("sum"))));
  auto symbol_table = MakeSymbolTable(*query);
  auto acc = ExpectAccumulate({symbol_table.at(*n_prop->expression_)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // We expect both the accumulation and aggregation because the part before
  // WITH updates the database.
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr,
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWithCreate) {
  // Test MATCH (n) WITH n AS a CREATE (a) -[r :r]-> (b)
  FakeDbAccessor dba;
  auto r_type = dba.EdgeType("r");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))), WITH("n", AS("a")),
      CREATE(
          PATTERN(NODE("a"), EDGE("r", Direction::OUT, {r_type}), NODE("b")))));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectProduce(),
                       ExpectCreateExpand());
}

TYPED_TEST(TestPlanner, MatchReturnSkipLimit) {
  // Test MATCH (n) RETURN n SKIP 2 LIMIT 1
  AstStorage storage;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                         RETURN(as_n, SKIP(LITERAL(2)), LIMIT(LITERAL(1)))));
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(),
            ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateWithSkipReturnLimit) {
  // Test CREATE (n) WITH n AS m SKIP 2 RETURN m LIMIT 1
  AstStorage storage;
  auto ident_n = IDENT("n");
  auto query = QUERY(SINGLE_QUERY(CREATE(PATTERN(NODE("n"))),
                                  WITH(ident_n, AS("m"), SKIP(LITERAL(2))),
                                  RETURN("m", LIMIT(LITERAL(1)))));
  auto symbol_table = MakeSymbolTable(*query);
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // Since we have a write query, we need to have Accumulate. This is a bit
  // different than Neo4j 3.0, which optimizes WITH followed by RETURN as a
  // single RETURN clause and then moves Skip and Limit before Accumulate.
  // This causes different behaviour. A newer version of Neo4j does the same
  // thing as us here (but who knows if they change it again).
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc,
            ExpectProduce(), ExpectSkip(), ExpectProduce(), ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateReturnSumSkipLimit) {
  // Test CREATE (n) RETURN SUM(n.prop) AS s SKIP 2 LIMIT 1
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto n_prop = PROPERTY_LOOKUP("n", prop);
  auto sum = SUM(n_prop);
  auto query = QUERY(
      SINGLE_QUERY(CREATE(PATTERN(NODE("n"))),
                   RETURN(sum, AS("s"), SKIP(LITERAL(2)), LIMIT(LITERAL(1)))));
  auto symbol_table = MakeSymbolTable(*query);
  auto acc = ExpectAccumulate({symbol_table.at(*n_prop->expression_)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr,
            ExpectProduce(), ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, MatchReturnOrderBy) {
  // Test MATCH (n) RETURN n AS m ORDER BY n.prop
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *as_m = NEXPR("m", IDENT("n"));
  auto *node_n = NODE("n");
  auto ret = RETURN(as_m, ORDER_BY(PROPERTY_LOOKUP("n", prop)));
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node_n)), ret));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectProduce(),
            ExpectOrderBy());
}

TYPED_TEST(TestPlanner, CreateWithOrderByWhere) {
  // Test CREATE (n) -[r :r]-> (m)
  //      WITH n AS new ORDER BY new.prop, r.prop WHERE m.prop < 42
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  auto r_type = dba.EdgeType("r");
  AstStorage storage;
  auto ident_n = IDENT("n");
  auto new_prop = PROPERTY_LOOKUP("new", prop);
  auto r_prop = PROPERTY_LOOKUP("r", prop);
  auto m_prop = PROPERTY_LOOKUP("m", prop);
  auto query = QUERY(SINGLE_QUERY(
      CREATE(
          PATTERN(NODE("n"), EDGE("r", Direction::OUT, {r_type}), NODE("m"))),
      WITH(ident_n, AS("new"), ORDER_BY(new_prop, r_prop)),
      WHERE(LESS(m_prop, LITERAL(42)))));
  auto symbol_table = MakeSymbolTable(*query);
  // Since this is a write query, we expect to accumulate to old used symbols.
  auto acc = ExpectAccumulate({
      symbol_table.at(*ident_n),              // `n` in WITH
      symbol_table.at(*r_prop->expression_),  // `r` in ORDER BY
      symbol_table.at(*m_prop->expression_),  // `m` in WHERE
  });
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(),
            ExpectCreateExpand(), acc, ExpectProduce(), ExpectOrderBy(),
            ExpectFilter());
}

TYPED_TEST(TestPlanner, ReturnAddSumCountOrderBy) {
  // Test RETURN SUM(1) + COUNT(2) AS result ORDER BY result
  AstStorage storage;
  auto sum = SUM(LITERAL(1));
  auto count = COUNT(LITERAL(2));
  auto *query = QUERY(SINGLE_QUERY(
      RETURN(ADD(sum, count), AS("result"), ORDER_BY(IDENT("result")))));
  auto aggr = ExpectAggregate({sum, count}, {});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce(), ExpectOrderBy());
}

TYPED_TEST(TestPlanner, MatchMerge) {
  // Test MATCH (n) MERGE (n) -[r :r]- (m)
  //      ON MATCH SET n.prop = 42 ON CREATE SET m = n
  //      RETURN n AS n
  FakeDbAccessor dba;
  auto r_type = dba.EdgeType("r");
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto ident_n = IDENT("n");
  auto query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      MERGE(PATTERN(NODE("n"), EDGE("r", Direction::BOTH, {r_type}), NODE("m")),
            ON_MATCH(SET(PROPERTY_LOOKUP("n", prop), LITERAL(42))),
            ON_CREATE(SET("m", IDENT("n")))),
      RETURN(ident_n, AS("n"))));
  std::list<BaseOpChecker *> on_match{new ExpectExpand(),
                                      new ExpectSetProperty()};
  std::list<BaseOpChecker *> on_create{new ExpectCreateExpand(),
                                       new ExpectSetProperties()};
  auto symbol_table = MakeSymbolTable(*query);
  // We expect Accumulate after Merge, because it is considered as a write.
  auto acc = ExpectAccumulate({symbol_table.at(*ident_n)});
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectMerge(on_match, on_create), acc, ExpectProduce());
  for (auto &op : on_match) delete op;
  on_match.clear();
  for (auto &op : on_create) delete op;
  on_create.clear();
}

TYPED_TEST(TestPlanner, MatchOptionalMatchWhereReturn) {
  // Test MATCH (n) OPTIONAL MATCH (n) -[r]- (m) WHERE m.prop < 42 RETURN r
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"))),
      OPTIONAL_MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      WHERE(LESS(PROPERTY_LOOKUP("m", prop), LITERAL(42))), RETURN("r")));
  std::list<BaseOpChecker *> optional{new ExpectScanAll(), new ExpectExpand(),
                                      new ExpectFilter()};
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(),
                       ExpectOptional(optional), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchUnwindReturn) {
  // Test MATCH (n) UNWIND [1,2,3] AS x RETURN n, x
  AstStorage storage;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *as_x = NEXPR("x", IDENT("x"));
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"))),
                   UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("x")),
                   RETURN(as_n, as_x)));
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectUnwind(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, ReturnDistinctOrderBySkipLimit) {
  // Test RETURN DISTINCT 1 ORDER BY 1 SKIP 1 LIMIT 1
  AstStorage storage;
  auto *query = QUERY(
      SINGLE_QUERY(RETURN_DISTINCT(LITERAL(1), AS("1"), ORDER_BY(LITERAL(1)),
                                   SKIP(LITERAL(1)), LIMIT(LITERAL(1)))));
  CheckPlan<TypeParam>(query, storage, ExpectProduce(), ExpectDistinct(),
                       ExpectOrderBy(), ExpectSkip(), ExpectLimit());
}

TYPED_TEST(TestPlanner, CreateWithDistinctSumWhereReturn) {
  // Test CREATE (n) WITH DISTINCT SUM(n.prop) AS s WHERE s < 42 RETURN s
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto node_n = NODE("n");
  auto sum = SUM(PROPERTY_LOOKUP("n", prop));
  auto query =
      QUERY(SINGLE_QUERY(CREATE(PATTERN(node_n)), WITH_DISTINCT(sum, AS("s")),
                         WHERE(LESS(IDENT("s"), LITERAL(42))), RETURN("s")));
  auto symbol_table = MakeSymbolTable(*query);
  auto acc = ExpectAccumulate({symbol_table.at(*node_n->identifier_)});
  auto aggr = ExpectAggregate({sum}, {});
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectCreateNode(), acc, aggr,
            ExpectProduce(), ExpectDistinct(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchCrossReferenceVariable) {
  // Test MATCH (n {prop: m.prop}), (m {prop: n.prop}) RETURN n
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR("prop");
  AstStorage storage;
  auto node_n = NODE("n");
  auto m_prop = PROPERTY_LOOKUP("m", prop.second);
  node_n->properties_[prop] = m_prop;
  auto node_m = NODE("m");
  auto n_prop = PROPERTY_LOOKUP("n", prop.second);
  node_m->properties_[prop] = n_prop;
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(node_n), PATTERN(node_m)), RETURN("n")));
  // We expect both ScanAll to come before filters (2 are joined into one),
  // because they need to populate the symbol values.
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectScanAll(),
                       ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWhereBeforeExpand) {
  // Test MATCH (n) -[r]- (m) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *as_n = NEXPR("n", IDENT("n"));
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      WHERE(LESS(PROPERTY_LOOKUP("n", prop), LITERAL(42))), RETURN(as_n)));
  // We expect Filter to come immediately after ScanAll, since it only uses `n`.
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectFilter(),
            ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiMatchWhere) {
  // Test MATCH (n) -[r]- (m) MATCH (l) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      MATCH(PATTERN(NODE("l"))),
      WHERE(LESS(PROPERTY_LOOKUP("n", prop), LITERAL(42))), RETURN("n")));
  // Even though WHERE is in the second MATCH clause, we expect Filter to come
  // before second ScanAll, since it only uses the value from first ScanAll.
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectFilter(),
                       ExpectExpand(), ExpectScanAll(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchOptionalMatchWhere) {
  // Test MATCH (n) -[r]- (m) OPTIONAL MATCH (l) WHERE n.prop < 42 RETURN n
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      OPTIONAL_MATCH(PATTERN(NODE("l"))),
      WHERE(LESS(PROPERTY_LOOKUP("n", prop), LITERAL(42))), RETURN("n")));
  // Even though WHERE is in the second MATCH clause, and it uses the value from
  // first ScanAll, it must remain part of the Optional. It should come before
  // optional ScanAll.
  std::list<BaseOpChecker *> optional{new ExpectFilter(), new ExpectScanAll()};
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpand(),
                       ExpectOptional(optional), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchReturnAsterisk) {
  // Test MATCH (n) -[e]- (m) RETURN *, m.prop
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto ret = RETURN(PROPERTY_LOOKUP("m", prop), AS("m.prop"));
  ret->body_.all_identifiers = true;
  auto query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("e"), NODE("m"))), ret));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectProduce());
  std::vector<std::string> output_names;
  for (const auto &output_symbol : planner.plan().OutputSymbols(symbol_table)) {
    output_names.emplace_back(output_symbol.name());
  }
  std::vector<std::string> expected_names{"e", "m", "n", "m.prop"};
  EXPECT_EQ(output_names, expected_names);
}

TYPED_TEST(TestPlanner, MatchReturnAsteriskSum) {
  // Test MATCH (n) RETURN *, SUM(n.prop) AS s
  FakeDbAccessor dba;
  auto prop = dba.Property("prop");
  AstStorage storage;
  auto sum = SUM(PROPERTY_LOOKUP("n", prop));
  auto ret = RETURN(sum, AS("s"));
  ret->body_.all_identifiers = true;
  auto query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"))), ret));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  auto *produce = dynamic_cast<Produce *>(&planner.plan());
  ASSERT_TRUE(produce);
  const auto &named_expressions = produce->named_expressions_;
  ASSERT_EQ(named_expressions.size(), 2);
  auto *expanded_ident =
      dynamic_cast<query::Identifier *>(named_expressions[0]->expression_);
  ASSERT_TRUE(expanded_ident);
  auto aggr = ExpectAggregate({sum}, {expanded_ident});
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), aggr,
            ExpectProduce());
  std::vector<std::string> output_names;
  for (const auto &output_symbol : planner.plan().OutputSymbols(symbol_table)) {
    output_names.emplace_back(output_symbol.name());
  }
  std::vector<std::string> expected_names{"n", "s"};
  EXPECT_EQ(output_names, expected_names);
}

TYPED_TEST(TestPlanner, UnwindMergeNodeProperty) {
  // Test UNWIND [1] AS i MERGE (n {prop: i})
  AstStorage storage;
  FakeDbAccessor dba;
  auto node_n = NODE("n");
  node_n->properties_[PROPERTY_PAIR("prop")] = IDENT("i");
  auto *query = QUERY(
      SINGLE_QUERY(UNWIND(LIST(LITERAL(1)), AS("i")), MERGE(PATTERN(node_n))));
  std::list<BaseOpChecker *> on_match{new ExpectScanAll(), new ExpectFilter()};
  std::list<BaseOpChecker *> on_create{new ExpectCreateNode()};
  CheckPlan<TypeParam>(query, storage, ExpectUnwind(),
                       ExpectMerge(on_match, on_create));
  for (auto &op : on_match) delete op;
  for (auto &op : on_create) delete op;
}

TYPED_TEST(TestPlanner, MultipleOptionalMatchReturn) {
  // Test OPTIONAL MATCH (n) OPTIONAL MATCH (m) RETURN n
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(OPTIONAL_MATCH(PATTERN(NODE("n"))),
                         OPTIONAL_MATCH(PATTERN(NODE("m"))), RETURN("n")));
  std::list<BaseOpChecker *> optional{new ExpectScanAll()};
  CheckPlan<TypeParam>(query, storage, ExpectOptional(optional),
                       ExpectOptional(optional), ExpectProduce());
}

TYPED_TEST(TestPlanner, FunctionAggregationReturn) {
  // Test RETURN sqrt(SUM(2)) AS result, 42 AS group_by
  AstStorage storage;
  auto sum = SUM(LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      RETURN(FN("sqrt", sum), AS("result"), group_by_literal, AS("group_by"))));
  auto aggr = ExpectAggregate({sum}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, FunctionWithoutArguments) {
  // Test RETURN pi() AS pi
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(RETURN(FN("pi"), AS("pi"))));
  CheckPlan<TypeParam>(query, storage, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListLiteralAggregationReturn) {
  // Test RETURN [SUM(2)] AS result, 42 AS group_by
  AstStorage storage;
  auto sum = SUM(LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      RETURN(LIST(sum), AS("result"), group_by_literal, AS("group_by"))));
  auto aggr = ExpectAggregate({sum}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapLiteralAggregationReturn) {
  // Test RETURN {sum: SUM(2)} AS result, 42 AS group_by
  AstStorage storage;
  FakeDbAccessor dba;
  auto sum = SUM(LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query =
      QUERY(SINGLE_QUERY(RETURN(MAP({PROPERTY_PAIR("sum"), sum}), AS("result"),
                                group_by_literal, AS("group_by"))));
  auto aggr = ExpectAggregate({sum}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, EmptyListIndexAggregation) {
  // Test RETURN [][SUM(2)] AS result, 42 AS group_by
  AstStorage storage;
  auto sum = SUM(LITERAL(2));
  auto empty_list = LIST();
  auto group_by_literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      RETURN(storage.Create<query::SubscriptOperator>(empty_list, sum),
             AS("result"), group_by_literal, AS("group_by"))));
  // We expect to group by '42' and the empty list, because it is a
  // sub-expression of a binary operator which contains an aggregation. This is
  // similar to grouping by '1' in `RETURN 1 + SUM(2)`.
  auto aggr = ExpectAggregate({sum}, {empty_list, group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListSliceAggregationReturn) {
  // Test RETURN [1, 2][0..SUM(2)] AS result, 42 AS group_by
  AstStorage storage;
  auto sum = SUM(LITERAL(2));
  auto list = LIST(LITERAL(1), LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query =
      QUERY(SINGLE_QUERY(RETURN(SLICE(list, LITERAL(0), sum), AS("result"),
                                group_by_literal, AS("group_by"))));
  // Similarly to EmptyListIndexAggregation test, we expect grouping by list and
  // '42', because slicing is an operator.
  auto aggr = ExpectAggregate({sum}, {list, group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, ListWithAggregationAndGroupBy) {
  // Test RETURN [sum(2), 42]
  AstStorage storage;
  auto sum = SUM(LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query =
      QUERY(SINGLE_QUERY(RETURN(LIST(sum, group_by_literal), AS("result"))));
  auto aggr = ExpectAggregate({sum}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, AggregatonWithListWithAggregationAndGroupBy) {
  // Test RETURN sum(2), [sum(3), 42]
  AstStorage storage;
  auto sum2 = SUM(LITERAL(2));
  auto sum3 = SUM(LITERAL(3));
  auto group_by_literal = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      RETURN(sum2, AS("sum2"), LIST(sum3, group_by_literal), AS("list"))));
  auto aggr = ExpectAggregate({sum2, sum3}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MapWithAggregationAndGroupBy) {
  // Test RETURN {lit: 42, sum: sum(2)}
  AstStorage storage;
  FakeDbAccessor dba;
  auto sum = SUM(LITERAL(2));
  auto group_by_literal = LITERAL(42);
  auto *query =
      QUERY(SINGLE_QUERY(RETURN(MAP({PROPERTY_PAIR("sum"), sum},
                                    {PROPERTY_PAIR("lit"), group_by_literal}),
                                AS("result"))));
  auto aggr = ExpectAggregate({sum}, {group_by_literal});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, CreateIndex) {
  // Test CREATE INDEX ON :Label(property)
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(CREATE_INDEX_ON(label, property)));
  CheckPlan<TypeParam>(query, storage, ExpectCreateIndex(label, property));
}

TYPED_TEST(TestPlanner, AtomIndexedLabelProperty) {
  // Test MATCH (n :label {property: 42, not_indexed: 0}) RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR("property");
  auto not_indexed = PROPERTY_PAIR("not_indexed");
  dba.SetIndexCount(label, 1);
  dba.SetIndexCount(label, property.second, 1);
  auto node = NODE("n", label);
  auto lit_42 = LITERAL(42);
  node->properties_[property] = lit_42;
  node->properties_[not_indexed] = LITERAL(0);
  auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(node)), RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelPropertyValue(label, property, lit_42),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, AtomPropertyWhereLabelIndexing) {
  // Test MATCH (n {property: 42}) WHERE n.not_indexed AND n:label RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR("property");
  auto not_indexed = PROPERTY_PAIR("not_indexed");
  dba.SetIndexCount(label, property.second, 0);
  auto node = NODE("n");
  auto lit_42 = LITERAL(42);
  node->properties_[property] = lit_42;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(node)),
      WHERE(AND(PROPERTY_LOOKUP("n", not_indexed),
                storage.Create<query::LabelsTest>(
                    IDENT("n"), std::vector<storage::Label>{label}))),
      RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelPropertyValue(label, property, lit_42),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, WhereIndexedLabelProperty) {
  // Test MATCH (n :label) WHERE n.property = 42 RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR("property");
  dba.SetIndexCount(label, property.second, 0);
  auto lit_42 = LITERAL(42);
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n", label))),
      WHERE(EQ(PROPERTY_LOOKUP("n", property), lit_42)), RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelPropertyValue(label, property, lit_42),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, BestPropertyIndexed) {
  // Test MATCH (n :label) WHERE n.property = 1 AND n.better = 42 RETURN n
  AstStorage storage;
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  // Add a vertex with :label+property combination, so that the best
  // :label+better remains empty and thus better choice.
  dba.SetIndexCount(label, property, 1);
  auto better = PROPERTY_PAIR("better");
  dba.SetIndexCount(label, better.second, 0);
  auto lit_42 = LITERAL(42);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n", label))),
                   WHERE(AND(EQ(PROPERTY_LOOKUP("n", property), LITERAL(1)),
                             EQ(PROPERTY_LOOKUP("n", better), lit_42))),
                   RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelPropertyValue(label, better, lit_42),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MultiPropertyIndexScan) {
  // Test MATCH (n :label1), (m :label2) WHERE n.prop1 = 1 AND m.prop2 = 2
  //      RETURN n, m
  FakeDbAccessor dba;
  auto label1 = dba.Label("label1");
  auto label2 = dba.Label("label2");
  auto prop1 = PROPERTY_PAIR("prop1");
  auto prop2 = PROPERTY_PAIR("prop2");
  dba.SetIndexCount(label1, prop1.second, 0);
  dba.SetIndexCount(label2, prop2.second, 0);
  AstStorage storage;
  auto lit_1 = LITERAL(1);
  auto lit_2 = LITERAL(2);
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n", label1)), PATTERN(NODE("m", label2))),
      WHERE(AND(EQ(PROPERTY_LOOKUP("n", prop1), lit_1),
                EQ(PROPERTY_LOOKUP("m", prop2), lit_2))),
      RETURN("n", "m")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table,
            ExpectScanAllByLabelPropertyValue(label1, prop1, lit_1),
            ExpectScanAllByLabelPropertyValue(label2, prop2, lit_2),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, WhereIndexedLabelPropertyRange) {
  // Test MATCH (n :label) WHERE n.property REL_OP 42 RETURN n
  // REL_OP is one of: `<`, `<=`, `>`, `>=`
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  dba.SetIndexCount(label, property, 0);
  AstStorage storage;
  auto lit_42 = LITERAL(42);
  auto n_prop = PROPERTY_LOOKUP("n", property);
  auto check_planned_range = [&label, &property, &dba](const auto &rel_expr,
                                                       auto lower_bound,
                                                       auto upper_bound) {
    // Shadow the first storage, so that the query is created in this one.
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n", label))),
                                     WHERE(rel_expr), RETURN("n")));
    auto symbol_table = MakeSymbolTable(*query);
    auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
    CheckPlan(planner.plan(), symbol_table,
              ExpectScanAllByLabelPropertyRange(label, property, lower_bound,
                                                upper_bound),
              ExpectProduce());
  };
  {
    // Test relation operators which form an upper bound for range.
    std::vector<std::pair<query::Expression *, Bound::Type>> upper_bound_rel_op{
        std::make_pair(LESS(n_prop, lit_42), Bound::Type::EXCLUSIVE),
        std::make_pair(LESS_EQ(n_prop, lit_42), Bound::Type::INCLUSIVE),
        std::make_pair(GREATER(lit_42, n_prop), Bound::Type::EXCLUSIVE),
        std::make_pair(GREATER_EQ(lit_42, n_prop), Bound::Type::INCLUSIVE)};
    for (const auto &rel_op : upper_bound_rel_op) {
      check_planned_range(rel_op.first, std::experimental::nullopt,
                          Bound(lit_42, rel_op.second));
    }
  }
  {
    // Test relation operators which form a lower bound for range.
    std::vector<std::pair<query::Expression *, Bound::Type>> lower_bound_rel_op{
        std::make_pair(LESS(lit_42, n_prop), Bound::Type::EXCLUSIVE),
        std::make_pair(LESS_EQ(lit_42, n_prop), Bound::Type::INCLUSIVE),
        std::make_pair(GREATER(n_prop, lit_42), Bound::Type::EXCLUSIVE),
        std::make_pair(GREATER_EQ(n_prop, lit_42), Bound::Type::INCLUSIVE)};
    for (const auto &rel_op : lower_bound_rel_op) {
      check_planned_range(rel_op.first, Bound(lit_42, rel_op.second),
                          std::experimental::nullopt);
    }
  }
}

TYPED_TEST(TestPlanner, UnableToUsePropertyIndex) {
  // Test MATCH (n: label) WHERE n.property = n.property RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  dba.SetIndexCount(label, property, 0);
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n", label))),
      WHERE(EQ(PROPERTY_LOOKUP("n", property), PROPERTY_LOOKUP("n", property))),
      RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // We can only get ScanAllByLabelIndex, because we are comparing properties
  // with those on the same node.
  CheckPlan(planner.plan(), symbol_table, ExpectScanAllByLabel(),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, SecondPropertyIndex) {
  // Test MATCH (n :label), (m :label) WHERE m.property = n.property RETURN n
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = PROPERTY_PAIR("property");
  dba.SetIndexCount(label, dba.Property("property"), 0);
  AstStorage storage;
  auto n_prop = PROPERTY_LOOKUP("n", property);
  auto m_prop = PROPERTY_LOOKUP("m", property);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n", label)), PATTERN(NODE("m", label))),
                   WHERE(EQ(m_prop, n_prop)), RETURN("n")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(
      planner.plan(), symbol_table, ExpectScanAllByLabel(),
      // Note: We are scanning for m, therefore property should equal n_prop.
      ExpectScanAllByLabelPropertyValue(label, property, n_prop),
      ExpectProduce());
}

TYPED_TEST(TestPlanner, ReturnSumGroupByAll) {
  // Test RETURN sum([1,2,3]), all(x in [1] where x = 1)
  AstStorage storage;
  auto sum = SUM(LIST(LITERAL(1), LITERAL(2), LITERAL(3)));
  auto *all = ALL("x", LIST(LITERAL(1)), WHERE(EQ(IDENT("x"), LITERAL(1))));
  auto *query = QUERY(SINGLE_QUERY(RETURN(sum, AS("sum"), all, AS("all"))));
  auto aggr = ExpectAggregate({sum}, {all});
  CheckPlan<TypeParam>(query, storage, aggr, ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariable) {
  // Test MATCH (n) -[r *..3]-> (m) RETURN r
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r");
  edge->upper_bound_ = LITERAL(3);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpandVariable(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableNoBounds) {
  // Test MATCH (n) -[r *]-> (m) RETURN r
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r");
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpandVariable(),
                       ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableInlinedFilter) {
  // Test MATCH (n) -[r :type * {prop: 42}]-> (m) RETURN r
  FakeDbAccessor dba;
  auto type = dba.EdgeType("type");
  auto prop = PROPERTY_PAIR("prop");
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::BOTH, {type});
  edge->properties_[prop] = LITERAL(42);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(
      query, storage, ExpectScanAll(),
      ExpectExpandVariable(),  // Filter is both inlined and post-expand
      ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchExpandVariableNotInlinedFilter) {
  // Test MATCH (n) -[r :type * {prop: m.prop}]-> (m) RETURN r
  FakeDbAccessor dba;
  auto type = dba.EdgeType("type");
  auto prop = PROPERTY_PAIR("prop");
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::BOTH, {type});
  edge->properties_[prop] = EQ(PROPERTY_LOOKUP("m", prop), LITERAL(42));
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectExpandVariable(),
                       ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, UnwindMatchVariable) {
  // Test UNWIND [1,2,3] AS depth MATCH (n) -[r*d]-> (m) RETURN r
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::OUT);
  edge->lower_bound_ = IDENT("d");
  edge->upper_bound_ = IDENT("d");
  auto *query = QUERY(
      SINGLE_QUERY(UNWIND(LIST(LITERAL(1), LITERAL(2), LITERAL(3)), AS("d")),
                   MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r")));
  CheckPlan<TypeParam>(query, storage, ExpectUnwind(), ExpectScanAll(),
                       ExpectExpandVariable(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchBfs) {
  // Test MATCH (n) -[r:type *..10 (r, n|n)]-> (m) RETURN r
  FakeDbAccessor dba;
  auto edge_type = dba.EdgeType("type");
  AstStorage storage;
  auto *bfs = storage.Create<query::EdgeAtom>(
      IDENT("r"), query::EdgeAtom::Type::BREADTH_FIRST, Direction::OUT,
      std::vector<storage::EdgeType>{edge_type});
  bfs->filter_lambda_.inner_edge = IDENT("r");
  bfs->filter_lambda_.inner_node = IDENT("n");
  bfs->filter_lambda_.expression = IDENT("n");
  bfs->upper_bound_ = LITERAL(10);
  auto *as_r = NEXPR("r", IDENT("r"));
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), bfs, NODE("m"))), RETURN(as_r)));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpandBfs(),
            ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchDoubleScanToExpandExisting) {
  // Test MATCH (n) -[r]- (m :label) RETURN r
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m", label))), RETURN("r")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // We expect 2x ScanAll and then Expand, since we are guessing that is
  // faster (due to low label index vertex count).
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(),
            ExpectScanAllByLabel(), ExpectExpand(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchScanToExpand) {
  // Test MATCH (n) -[r]- (m :label {property: 1}) RETURN r
  FakeDbAccessor dba;
  auto label = dba.Label("label");
  auto property = dba.Property("property");
  // Fill vertices to the max + 1.
  dba.SetIndexCount(label, property,
                    FLAGS_query_vertex_count_to_expand_existing + 1);
  dba.SetIndexCount(label, FLAGS_query_vertex_count_to_expand_existing + 1);
  AstStorage storage;
  auto node_m = NODE("m", label);
  node_m->properties_[std::make_pair("property", property)] = LITERAL(1);
  auto *query = QUERY(
      SINGLE_QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), node_m)), RETURN("r")));
  auto symbol_table = MakeSymbolTable(*query);
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  // We expect 1x ScanAll and then Expand, since we are guessing that
  // is faster (due to high label index vertex count).
  CheckPlan(planner.plan(), symbol_table, ExpectScanAll(), ExpectExpand(),
            ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, MatchWhereAndSplit) {
  // Test MATCH (n) -[r]- (m) WHERE n.prop AND r.prop RETURN m
  FakeDbAccessor dba;
  auto prop = PROPERTY_PAIR("prop");
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
      WHERE(AND(PROPERTY_LOOKUP("n", prop), PROPERTY_LOOKUP("r", prop))),
      RETURN("m")));
  // We expect `n.prop` filter right after scanning `n`.
  CheckPlan<TypeParam>(query, storage, ExpectScanAll(), ExpectFilter(),
                       ExpectExpand(), ExpectFilter(), ExpectProduce());
}

TYPED_TEST(TestPlanner, ReturnAsteriskOmitsLambdaSymbols) {
  // Test MATCH (n) -[r* (ie, in | true)]- (m) RETURN *
  AstStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::BOTH);
  edge->filter_lambda_.inner_edge = IDENT("ie");
  edge->filter_lambda_.inner_node = IDENT("in");
  edge->filter_lambda_.expression = LITERAL(true);
  auto ret = storage.Create<query::Return>();
  ret->body_.all_identifiers = true;
  auto *query =
      QUERY(SINGLE_QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), ret));
  auto symbol_table = MakeSymbolTable(*query);
  FakeDbAccessor dba;
  auto planner = MakePlanner<TypeParam>(dba, storage, symbol_table, query);
  auto *produce = dynamic_cast<Produce *>(&planner.plan());
  ASSERT_TRUE(produce);
  std::vector<std::string> outputs;
  for (const auto &output_symbol : produce->OutputSymbols(symbol_table)) {
    outputs.emplace_back(output_symbol.name());
  }
  // We expect `*` expanded to `n`, `r` and `m`.
  EXPECT_EQ(outputs.size(), 3);
  for (const auto &name : {"n", "r", "m"}) {
    EXPECT_TRUE(utils::Contains(outputs, name));
  }
}

TYPED_TEST(TestPlanner, AuthQuery) {
  // Check if everything is properly forwarded from ast node to the operator
  FakeDbAccessor dba;
  AstStorage storage;
  auto *query =
      QUERY(SINGLE_QUERY(AUTH_QUERY(query::AuthQuery::Action::DROP_ROLE, "user",
                                    "role", "user_or_role", LITERAL("password"),
                                    std::vector<query::AuthQuery::Privilege>(
                                        {query::AuthQuery::Privilege::MATCH,
                                         query::AuthQuery::Privilege::AUTH}))));
  CheckPlan<TypeParam>(
      query, storage,
      ExpectAuthHandler(query::AuthQuery::Action::DROP_ROLE, "user", "role",
                        "user_or_role", LITERAL("password"),
                        {query::AuthQuery::Privilege::MATCH,
                         query::AuthQuery::Privilege::AUTH}));
}

TYPED_TEST(TestPlanner, CreateStream) {
  std::string stream_name("kafka"), stream_uri("localhost:1234"),
      stream_topic("tropik"), transform_uri("localhost:1234/file.py");
  int64_t batch_interval_in_ms = 100;
  int64_t batch_size = 10;
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query =
        QUERY(SINGLE_QUERY(CREATE_STREAM(stream_name, stream_uri, stream_topic,
                                         transform_uri, nullptr, nullptr)));
    auto expected = ExpectCreateStream(
        stream_name, LITERAL(stream_uri), LITERAL(stream_topic),
        LITERAL(transform_uri), nullptr, nullptr);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(
        CREATE_STREAM(stream_name, stream_uri, stream_topic, transform_uri,
                      LITERAL(batch_interval_in_ms), nullptr)));
    auto expected = ExpectCreateStream(
        stream_name, LITERAL(stream_uri), LITERAL(stream_topic),
        LITERAL(transform_uri), LITERAL(batch_interval_in_ms), nullptr);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(
        CREATE_STREAM(stream_name, stream_uri, stream_topic, transform_uri,
                      nullptr, LITERAL(batch_size))));
    auto expected = ExpectCreateStream(
        stream_name, LITERAL(stream_uri), LITERAL(stream_topic),
        LITERAL(transform_uri), nullptr, LITERAL(batch_size));
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(
        CREATE_STREAM(stream_name, stream_uri, stream_topic, transform_uri,
                      LITERAL(batch_interval_in_ms), LITERAL(batch_size))));
    auto expected =
        ExpectCreateStream(stream_name, LITERAL(stream_uri),
                           LITERAL(stream_topic), LITERAL(transform_uri),
                           LITERAL(batch_interval_in_ms), LITERAL(batch_size));
    CheckPlan<TypeParam>(query, storage, expected);
  }
}

TYPED_TEST(TestPlanner, DropStream) {
  std::string stream_name("kafka");
  FakeDbAccessor dba;
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(DROP_STREAM(stream_name)));
  auto expected = ExpectDropStream(stream_name);
  CheckPlan<TypeParam>(query, storage, expected);
}

TYPED_TEST(TestPlanner, ShowStreams) {
  FakeDbAccessor dba;
  AstStorage storage;
  auto *query = QUERY(SINGLE_QUERY(SHOW_STREAMS));
  auto expected = ExpectShowStreams();
  CheckPlan<TypeParam>(query, storage, expected);
}

TYPED_TEST(TestPlanner, StartStopStream) {
  std::string stream_name("kafka");
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(START_STREAM(stream_name, nullptr)));
    auto expected = ExpectStartStopStream(stream_name, true, nullptr);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto limit_batches = LITERAL(10);
    auto *query = QUERY(SINGLE_QUERY(START_STREAM(stream_name, limit_batches)));
    auto expected = ExpectStartStopStream(stream_name, true, limit_batches);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(STOP_STREAM(stream_name)));
    auto expected = ExpectStartStopStream(stream_name, false, nullptr);
    CheckPlan<TypeParam>(query, storage, expected);
  }
}

TYPED_TEST(TestPlanner, StartStopAllStreams) {
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(START_ALL_STREAMS));
    auto expected = ExpectStartStopAllStreams(true);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(STOP_ALL_STREAMS));
    auto expected = ExpectStartStopAllStreams(false);
    CheckPlan<TypeParam>(query, storage, expected);
  }
}

TYPED_TEST(TestPlanner, TestStream) {
  std::string stream_name("kafka");
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto *query = QUERY(SINGLE_QUERY(TEST_STREAM(stream_name, nullptr)));
    auto expected = ExpectTestStream(stream_name, nullptr);
    CheckPlan<TypeParam>(query, storage, expected);
  }
  {
    FakeDbAccessor dba;
    AstStorage storage;
    auto limit_batches = LITERAL(10);
    auto *query = QUERY(SINGLE_QUERY(TEST_STREAM(stream_name, limit_batches)));
    auto expected = ExpectTestStream(stream_name, limit_batches);
    CheckPlan<TypeParam>(query, storage, expected);
  }
}

}  // namespace