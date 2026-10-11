// Unit tests for the pure helper functions in neo4j_threaded_query_union_delimitter.cpp.
// The source file is included directly; UNIT_TEST (set by the Makefile) leaves out its main().
// sendQuery() needs a running Neo4j server and is not covered here.
#include <gtest/gtest.h>

#include "../neo4j_threaded_query_union_delimitter.cpp"

using Strings = std::vector<std::string>;
using nlohmann::json;

// Flattens splitQuery's output into the list of branches, in order
Strings branchesOf(const Strings& statements) {
    Strings branches;
    for (const auto& statement : splitQuery(statements)) {
        branches.insert(branches.end(), statement.branches.begin(), statement.branches.end());
    }
    return branches;
}

// Builds a successful single-statement Neo4j response from rows of values
json neo4jResponse(const json& columns, const json& rows, const json& metas = json()) {
    json data = json::array();
    for (size_t i = 0; i < rows.size(); ++i) {
        data.push_back({{"row", rows[i]}, {"meta", metas.is_null() ? json::array({nullptr}) : metas[i]}});
    }
    return {{"results", {{{"columns", columns}, {"data", data}}}}, {"errors", json::array()}};
}

// Extracts just the row values from a merged result
json rowsOf(const json& merged) {
    json rows = json::array();
    for (const auto& entry : merged["data"]) rows.push_back(entry["row"]);
    return rows;
}

// ---------------------------------------------------------------------------
// splitQuery
// ---------------------------------------------------------------------------

TEST(SplitQuery, NoUnionReturnsStatementUnchanged) {
    EXPECT_EQ(branchesOf({"MATCH (p:Person) RETURN p"}), Strings({"MATCH (p:Person) RETURN p"}));
}

TEST(SplitQuery, SplitsOnUnion) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x UNION RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, SplitsOnMultipleUnions) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x UNION RETURN 2 AS x UNION RETURN 3 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x", "RETURN 3 AS x"}));
}

TEST(SplitQuery, ConsumesUnionAll) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x UNION ALL RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, UnionAllAcrossNewlines) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x\nUNION\n  ALL\nRETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, KeywordsAreCaseInsensitive) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x union RETURN 2 AS x Union RETURN 3 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x", "RETURN 3 AS x"}));
    EXPECT_EQ(branchesOf({"RETURN 1 AS x union all RETURN 2 AS x"}), Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, UnionIsDistinctUnionAllIsNot) {
    EXPECT_TRUE(splitStatement("RETURN 1 AS x UNION RETURN 2 AS x").distinct);
    EXPECT_FALSE(splitStatement("RETURN 1 AS x UNION ALL RETURN 2 AS x").distinct);
    EXPECT_FALSE(splitStatement("RETURN 1 AS x").distinct);
}

TEST(SplitQuery, MixedUnionAndUnionAllIsNotSplit) {
    // Neo4j rejects this mix; send it unchanged so Neo4j reports the error
    const std::string q = "RETURN 1 AS x UNION ALL RETURN 2 AS x UNION RETURN 3 AS x";
    SplitStatement split = splitStatement(q);
    EXPECT_EQ(split.branches, Strings({q}));
    EXPECT_FALSE(split.distinct);
}

TEST(SplitQuery, WriteStatementIsNotSplit) {
    const std::string q = "CREATE (n:Person {name: 'Alice'}) RETURN n AS person UNION CREATE (m:Person {name: 'Bob'}) RETURN m AS person";
    EXPECT_EQ(branchesOf({q}), Strings({q}));
}

TEST(SplitQuery, AllPrefixedIdentifierIsNotTreatedAsAll) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x UNION allNodes RETURN 2"}),
              Strings({"RETURN 1 AS x", "allNodes RETURN 2"}));
}

TEST(SplitQuery, DoesNotSplitOnIntersect) {
    // Cypher has no INTERSECT; only UNION / UNION ALL are split points
    const std::string q = "RETURN 1 AS x INTERSECT RETURN 2 AS x";
    EXPECT_EQ(branchesOf({q}), Strings({q}));
}

TEST(SplitQuery, IgnoresUnionInSingleQuotedString) {
    EXPECT_EQ(branchesOf({"RETURN 'a UNION b' AS x"}), Strings({"RETURN 'a UNION b' AS x"}));
}

TEST(SplitQuery, IgnoresUnionInDoubleQuotedStringWithEscapes) {
    const std::string q = R"(RETURN "it\"s a UNION" AS x)";
    EXPECT_EQ(branchesOf({q}), Strings({q}));
}

TEST(SplitQuery, IgnoresUnionInBacktickIdentifier) {
    EXPECT_EQ(branchesOf({"MATCH (n:`UNION`) RETURN n"}), Strings({"MATCH (n:`UNION`) RETURN n"}));
}

TEST(SplitQuery, IgnoresUnionAsPartOfLongerWord) {
    const std::string q = "MATCH (n:Reunion) RETURN n.unionized, $union_id";
    EXPECT_EQ(branchesOf({q}), Strings({q}));
}

TEST(SplitQuery, IgnoresPropertyNamedUnion) {
    EXPECT_EQ(branchesOf({"MATCH (n) RETURN n.union AS u"}), Strings({"MATCH (n) RETURN n.union AS u"}));
}

TEST(SplitQuery, IgnoresUnionInsideCallSubquery) {
    const std::string q = "CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x";
    EXPECT_EQ(branchesOf({q}), Strings({q}));
}

TEST(SplitQuery, SplitsOutsideButNotInsideCallSubquery) {
    EXPECT_EQ(branchesOf({"CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x UNION RETURN 3 AS x"}),
              Strings({"CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x", "RETURN 3 AS x"}));
}

TEST(SplitQuery, IgnoresUnionInComments) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x // UNION here\nUNION /* UNION */ RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x // UNION here", "/* UNION */ RETURN 2 AS x"}));
}

TEST(SplitQuery, UnterminatedStringDoesNotCrash) {
    EXPECT_EQ(branchesOf({"RETURN 'oops UNION"}), Strings({"RETURN 'oops UNION"}));
}

TEST(SplitQuery, TrimsWhitespaceAndDropsEmptyBranches) {
    EXPECT_EQ(branchesOf({"  UNION RETURN 1 AS x  UNION  "}), Strings({"RETURN 1 AS x"}));
}

TEST(SplitQuery, EmptyInput) {
    EXPECT_TRUE(branchesOf({}).empty());
    EXPECT_TRUE(branchesOf({""}).empty());
    EXPECT_TRUE(branchesOf({"   \n\t"}).empty());
}

TEST(SplitQuery, HandlesMultipleStatementsInOrder) {
    EXPECT_EQ(branchesOf({"RETURN 1 AS x UNION RETURN 2 AS x", "MATCH (p) RETURN p"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x", "MATCH (p) RETURN p"}));
}

// ---------------------------------------------------------------------------
// matchKeyword / isIdentChar / trim
// ---------------------------------------------------------------------------

TEST(MatchKeyword, MatchesWholeWordOnly) {
    EXPECT_TRUE(matchKeyword("UNION", 0, "UNION"));
    EXPECT_TRUE(matchKeyword("a UNION b", 2, "UNION"));
    EXPECT_TRUE(matchKeyword("a union b", 2, "UNION"));
    EXPECT_FALSE(matchKeyword("UNIONS", 0, "UNION"));
    EXPECT_FALSE(matchKeyword("REUNION", 2, "UNION"));
    EXPECT_FALSE(matchKeyword("n.union", 2, "UNION"));
    EXPECT_FALSE(matchKeyword("$union", 1, "UNION"));
}

TEST(MatchKeyword, OutOfRangePositionIsNoMatch) {
    EXPECT_FALSE(matchKeyword("UNI", 0, "UNION"));
    EXPECT_FALSE(matchKeyword("UNION", 3, "UNION"));
    EXPECT_FALSE(matchKeyword("", 0, "UNION"));
}

TEST(IsIdentChar, Classification) {
    for (char c : std::string("azAZ09_$")) EXPECT_TRUE(isIdentChar(c)) << c;
    for (char c : std::string(" .({`'\"\n-")) EXPECT_FALSE(isIdentChar(c)) << c;
}

TEST(Trim, StripsSurroundingWhitespace) {
    EXPECT_EQ(trim("  a b \t\r\n"), "a b");
    EXPECT_EQ(trim("abc"), "abc");
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim(" \n\t "), "");
}

// ---------------------------------------------------------------------------
// isWriteQuery
// ---------------------------------------------------------------------------

TEST(IsWriteQuery, DetectsWriteClauses) {
    EXPECT_TRUE(isWriteQuery("CREATE (n:Person)"));
    EXPECT_TRUE(isWriteQuery("MERGE (n:Person {name: 'A'})"));
    EXPECT_TRUE(isWriteQuery("MATCH (n) DETACH DELETE n"));
    EXPECT_TRUE(isWriteQuery("MATCH (n) set n.x = 1"));
    EXPECT_TRUE(isWriteQuery("MATCH (n) REMOVE n:Label"));
    EXPECT_TRUE(isWriteQuery("FOREACH (x IN [1] | CREATE (:N))"));
    EXPECT_TRUE(isWriteQuery("CALL { CREATE (n) } RETURN 1"));  // nested writes count too
}

TEST(IsWriteQuery, ReadQueriesAreNotWrites) {
    EXPECT_FALSE(isWriteQuery("MATCH (n) RETURN n"));
    EXPECT_FALSE(isWriteQuery("RETURN 'CREATE something' AS s"));       // in a string
    EXPECT_FALSE(isWriteQuery("MATCH (n:`SET`) RETURN n"));              // quoted label
    EXPECT_FALSE(isWriteQuery("MATCH (n) RETURN n.set, n.created"));     // property names
    EXPECT_FALSE(isWriteQuery("MATCH (n) RETURN n // DELETE later"));    // comment
    EXPECT_FALSE(isWriteQuery("MATCH (n:Dataset) RETURN $offset"));      // longer words
}

// ---------------------------------------------------------------------------
// mergeBranchResults
// ---------------------------------------------------------------------------

TEST(MergeBranchResults, UnionAllKeepsDuplicatesInBranchOrder) {
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"x"}, {{1}, {2}}), neo4jResponse({"x"}, {{2}, {3}})}, false, errors);
    EXPECT_EQ(merged["columns"], json({"x"}));
    EXPECT_EQ(rowsOf(merged), json({{1}, {2}, {2}, {3}}));
    EXPECT_TRUE(errors.empty());
}

TEST(MergeBranchResults, UnionRemovesDuplicateRows) {
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"x"}, {{1}, {2}, {1}}), neo4jResponse({"x"}, {{2}, {3}})}, true, errors);
    EXPECT_EQ(rowsOf(merged), json({{1}, {2}, {3}}));
    EXPECT_TRUE(errors.empty());
}

TEST(MergeBranchResults, UnionKeepsDifferentNodesWithEqualProperties) {
    // Two distinct Alice nodes: same property values, different elementId
    json alice = {{"name", "Alice"}};
    json meta1 = json::array({{{"elementId", "4:db:1"}, {"type", "node"}}});
    json meta2 = json::array({{{"elementId", "4:db:2"}, {"type", "node"}}});
    json errors = json::array();

    json merged = mergeBranchResults({neo4jResponse({"p"}, {{alice}}, json::array({meta1})),
                                      neo4jResponse({"p"}, {{alice}, {alice}}, json::array({meta2, meta1}))},
                                     true, errors);
    EXPECT_EQ(merged["data"].size(), 2u);  // node 1 once, node 2 once
}

TEST(MergeBranchResults, ColumnMismatchIsAnError) {
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"x"}, {{1}}), neo4jResponse({"y"}, {{2}})}, false, errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["code"], "Client.ColumnMismatch");
    EXPECT_TRUE(merged["data"].empty());
}

TEST(MergeBranchResults, Neo4jErrorsArePassedThrough) {
    json failed = {{"results", json::array()},
                   {"errors", {{{"code", "Neo.ClientError.Statement.SyntaxError"}, {"message", "bad"}}}}};
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"x"}, {{1}}), failed}, false, errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["code"], "Neo.ClientError.Statement.SyntaxError");
    EXPECT_TRUE(merged["data"].empty());  // no partial results
}

TEST(MergeBranchResults, MissingResponseIsAnError) {
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"x"}, {{1}}), json()}, false, errors);  // e.g. request failed
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["code"], "Client.RequestFailed");
    EXPECT_TRUE(merged["data"].empty());
}

TEST(MergeBranchResults, SingleBranchPassesThrough) {
    json errors = json::array();
    json merged = mergeBranchResults({neo4jResponse({"people"}, {{3}})}, false, errors);
    EXPECT_EQ(merged["columns"], json({"people"}));
    EXPECT_EQ(rowsOf(merged), json({{3}}));
}

// ---------------------------------------------------------------------------
// extractStatements
// ---------------------------------------------------------------------------

TEST(ExtractStatements, ReturnsStatementsInOrder) {
    auto json_query = nlohmann::json::parse(R"({
        "statements": [ {"statement": "RETURN 1"}, {"statement": "MATCH (n) RETURN n"} ]
    })");
    EXPECT_EQ(extractStatements(json_query), Strings({"RETURN 1", "MATCH (n) RETURN n"}));
}

TEST(ExtractStatements, EmptyStatementsArray) {
    EXPECT_TRUE(extractStatements(nlohmann::json::parse(R"({"statements": []})")).empty());
}

TEST(ExtractStatements, NonStringStatementThrows) {
    EXPECT_THROW(extractStatements(nlohmann::json::parse(R"({"statements": [{"statement": 42}]})")),
                 nlohmann::json::type_error);
}

// ---------------------------------------------------------------------------
// buildRequest
// ---------------------------------------------------------------------------

TEST(BuildRequest, ProducesNeo4jRequestShape) {
    EXPECT_EQ(buildRequest("RETURN 1 AS x"), R"({"statements":[{"statement":"RETURN 1 AS x"}]})");
}

TEST(BuildRequest, EscapesQuotesBackslashesAndNewlines) {
    const std::string subquery = "RETURN \"say \\\"hi\\\"\" AS x, 'a\\\\b' AS y\nRETURN 2";
    const std::string body = buildRequest(subquery);

    nlohmann::json parsed;
    ASSERT_NO_THROW(parsed = nlohmann::json::parse(body));
    EXPECT_EQ(parsed["statements"][0]["statement"].get<std::string>(), subquery);
    EXPECT_EQ(body.find('\n'), std::string::npos);  // newline must be escaped, not raw
}

TEST(BuildRequest, RoundTripsEverySplitBranch) {
    auto json_query = nlohmann::json::parse(R"({"statements": [
        {"statement": "MATCH (n:Person {name: 'Alice'}) RETURN n AS person UNION MATCH (m:Person {name: \"Bob\"}) RETURN m AS person"}
    ]})");
    for (const auto& subquery : branchesOf(extractStatements(json_query))) {
        auto parsed = nlohmann::json::parse(buildRequest(subquery));
        EXPECT_EQ(parsed["statements"][0]["statement"].get<std::string>(), subquery);
    }
}

// ---------------------------------------------------------------------------
// processSingleResponse
// ---------------------------------------------------------------------------

TEST(ProcessSingleResponse, ParsesValidJson) {
    auto json = processSingleResponse(R"({"results":[{"columns":["x"],"data":[{"row":[1]}]}],"errors":[]})");
    EXPECT_EQ(json["results"][0]["data"][0]["row"][0], 1);
}

TEST(ProcessSingleResponse, InvalidJsonReturnsNull) {
    EXPECT_TRUE(processSingleResponse("not json").is_null());
}

TEST(ProcessSingleResponse, EmptyResponseReturnsNull) {
    // sendQuery() returns "" when the request fails
    EXPECT_TRUE(processSingleResponse("").is_null());
}

// ---------------------------------------------------------------------------
// base64_encode
// ---------------------------------------------------------------------------

TEST(Base64Encode, EncodesDefaultCredentials) {
    EXPECT_EQ(base64_encode("neo4j:testpassword"), "bmVvNGo6dGVzdHBhc3N3b3Jk");
}

TEST(Base64Encode, EncodesWithPadding) {
    EXPECT_EQ(base64_encode("a"), "YQ==");
}

// KNOWN BUG: OpenSSL's base64 BIO inserts a newline every 64 output chars, so
// credentials longer than 48 bytes produce an Authorization header with a raw
// newline in it. Enable once base64_encode sets BIO_FLAGS_BASE64_NO_NL.
TEST(Base64Encode, DISABLED_LongInputHasNoEmbeddedNewline) {
    EXPECT_EQ(base64_encode("user_with_a_rather_long_name:and_an_even_longer_password_123"),
              "dXNlcl93aXRoX2FfcmF0aGVyX2xvbmdfbmFtZTphbmRfYW5fZXZlbl9sb25nZXJfcGFzc3dvcmRfMTIz");
}

// KNOWN BUG: for empty input OpenSSL writes nothing, so `bufferPtr->length - 1`
// underflows and the std::string constructor throws/crashes. Enable once fixed.
TEST(Base64Encode, DISABLED_EmptyInput) {
    EXPECT_EQ(base64_encode(""), "");
}
