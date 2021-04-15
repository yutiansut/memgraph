#include "query/cypher_query_interpreter.hpp"

// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_HIDDEN_bool(query_cost_planner, true, "Use the cost-estimating query planner.");
// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_int32(query_plan_cache_ttl, 60, "Time to live for cached query plans, in seconds.",
                       FLAG_IN_RANGE(0, std::numeric_limits<int32_t>::max()));

namespace query {
CachedPlan::CachedPlan(std::unique_ptr<LogicalPlan> plan) : plan_(std::move(plan)) {}

ParsedQuery ParseQuery(const std::string &query_string, const std::map<std::string, storage::PropertyValue> &params,
                       utils::SkipList<QueryCacheEntry> *cache, utils::SpinLock *antlr_lock) {
  // Strip the query for caching purposes. The process of stripping a query
  // "normalizes" it by replacing any literals with new parameters. This
  // results in just the *structure* of the query being taken into account for
  // caching.
  frontend::StrippedQuery stripped_query{query_string};

  // Copy over the parameters that were introduced during stripping.
  Parameters parameters{stripped_query.literals()};

  // Check that all user-specified parameters are provided.
  for (const auto &param_pair : stripped_query.parameters()) {
    auto it = params.find(param_pair.second);

    if (it == params.end()) {
      throw query::UnprovidedParameterError("Parameter ${} not provided.", param_pair.second);
    }

    parameters.Add(param_pair.first, it->second);
  }

  // Cache the query's AST if it isn't already.
  auto hash = stripped_query.hash();
  auto accessor = cache->access();
  auto it = accessor.find(hash);
  std::unique_ptr<frontend::opencypher::Parser> parser;

  // Return a copy of both the AST storage and the query.
  CachedQuery result;
  bool is_cacheable = true;

  auto get_information_from_cache = [&](const auto &cached_query) {
    result.ast_storage.properties_ = cached_query.ast_storage.properties_;
    result.ast_storage.labels_ = cached_query.ast_storage.labels_;
    result.ast_storage.edge_types_ = cached_query.ast_storage.edge_types_;

    result.query = cached_query.query->Clone(&result.ast_storage);
    result.required_privileges = cached_query.required_privileges;
  };

  if (it == accessor.end()) {
    {
      std::unique_lock<utils::SpinLock> guard(*antlr_lock);

      try {
        parser = std::make_unique<frontend::opencypher::Parser>(stripped_query.query());
      } catch (const SyntaxException &e) {
        // There is a syntax exception in the stripped query. Re-run the parser
        // on the original query to get an appropriate error messsage.
        parser = std::make_unique<frontend::opencypher::Parser>(query_string);

        // If an exception was not thrown here, the stripper messed something
        // up.
        LOG_FATAL("The stripped query can't be parsed, but the original can.");
      }
    }

    // Convert the ANTLR4 parse tree into an AST.
    AstStorage ast_storage;
    frontend::ParsingContext context{true};
    frontend::CypherMainVisitor visitor(context, &ast_storage);

    visitor.visit(parser->tree());

    if (visitor.IsCacheable()) {
      CachedQuery cached_query{std::move(ast_storage), visitor.query(), query::GetRequiredPrivileges(visitor.query())};
      it = accessor.insert({hash, std::move(cached_query)}).first;

      get_information_from_cache(it->second);
    } else {
      result.ast_storage.properties_ = ast_storage.properties_;
      result.ast_storage.labels_ = ast_storage.labels_;
      result.ast_storage.edge_types_ = ast_storage.edge_types_;

      result.query = visitor.query()->Clone(&result.ast_storage);
      result.required_privileges = query::GetRequiredPrivileges(visitor.query());

      is_cacheable = false;
    }
  } else {
    get_information_from_cache(it->second);
  }

  return ParsedQuery{query_string,
                     params,
                     std::move(parameters),
                     std::move(stripped_query),
                     std::move(result.ast_storage),
                     result.query,
                     std::move(result.required_privileges),
                     is_cacheable};
}

std::unique_ptr<LogicalPlan> MakeLogicalPlan(AstStorage ast_storage, CypherQuery *query, const Parameters &parameters,
                                             DbAccessor *db_accessor) {
  auto vertex_counts = plan::MakeVertexCountCache(db_accessor);
  auto symbol_table = MakeSymbolTable(query);
  auto planning_context = plan::MakePlanningContext(&ast_storage, &symbol_table, query, &vertex_counts);
  auto [root, cost] = plan::MakeLogicalPlan(&planning_context, parameters, FLAGS_query_cost_planner);
  return std::make_unique<SingleNodeLogicalPlan>(std::move(root), cost, std::move(ast_storage),
                                                 std::move(symbol_table));
}

std::shared_ptr<CachedPlan> CypherQueryToPlan(uint64_t hash, AstStorage ast_storage, CypherQuery *query,
                                              const Parameters &parameters, utils::SkipList<PlanCacheEntry> *plan_cache,
                                              DbAccessor *db_accessor, const bool is_cacheable) {
  auto plan_cache_access = plan_cache->access();
  auto it = plan_cache_access.find(hash);
  if (it != plan_cache_access.end()) {
    if (it->second->IsExpired()) {
      plan_cache_access.remove(hash);
    } else {
      return it->second;
    }
  }

  auto plan = std::make_shared<CachedPlan>(MakeLogicalPlan(std::move(ast_storage), (query), parameters, db_accessor));
  if (is_cacheable) {
    plan_cache_access.insert({hash, plan});
  }
  return plan;
}
}  // namespace query