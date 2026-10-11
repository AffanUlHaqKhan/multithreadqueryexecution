// Unit tests for the pure helper functions in neo4j_threaded_query_union_delimitter.cpp.
// The source file is included directly; UNIT_TEST (set by the Makefile) leaves out its main().
// sendQuery() needs a running Neo4j server and is not covered here.
#include <gtest/gtest.h>

#include "../neo4j_threaded_query_union_delimitter.cpp"

using Strings = std::vector<std::string>;

// ---------------------------------------------------------------------------
// splitQuery
// ---------------------------------------------------------------------------

TEST(SplitQuery, NoUnionReturnsStatementUnchanged) {
    EXPECT_EQ(splitQuery({"MATCH (p:Person) RETURN p"}), Strings({"MATCH (p:Person) RETURN p"}));
}

TEST(SplitQuery, SplitsOnUnion) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x UNION RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, SplitsOnMultipleUnions) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x UNION RETURN 2 AS x UNION RETURN 3 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x", "RETURN 3 AS x"}));
}

TEST(SplitQuery, ConsumesUnionAll) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x UNION ALL RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, UnionAllAcrossNewlines) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x\nUNION\n  ALL\nRETURN 2 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x"}));
}

TEST(SplitQuery, KeywordsAreCaseInsensitive) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x union all RETURN 2 AS x Union RETURN 3 AS x"}),
              Strings({"RETURN 1 AS x", "RETURN 2 AS x", "RETURN 3 AS x"}));
}

TEST(SplitQuery, AllPrefixedIdentifierIsNotTreatedAsAll) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x UNION allNodes RETURN 2"}),
              Strings({"RETURN 1 AS x", "allNodes RETURN 2"}));
}

TEST(SplitQuery, IgnoresUnionInSingleQuotedString) {
    EXPECT_EQ(splitQuery({"RETURN 'a UNION b' AS x"}), Strings({"RETURN 'a UNION b' AS x"}));
}

TEST(SplitQuery, IgnoresUnionInDoubleQuotedStringWithEscapes) {
    const std::string q = R"(RETURN "it\"s a UNION" AS x)";
    EXPECT_EQ(splitQuery({q}), Strings({q}));
}

TEST(SplitQuery, IgnoresUnionInBacktickIdentifier) {
    EXPECT_EQ(splitQuery({"MATCH (n:`UNION`) RETURN n"}), Strings({"MATCH (n:`UNION`) RETURN n"}));
}

TEST(SplitQuery, IgnoresUnionAsPartOfLongerWord) {
    const std::string q = "MATCH (n:Reunion) RETURN n.unionized, $union_id";
    EXPECT_EQ(splitQuery({q}), Strings({q}));
}

TEST(SplitQuery, IgnoresPropertyNamedUnion) {
    EXPECT_EQ(splitQuery({"MATCH (n) RETURN n.union AS u"}), Strings({"MATCH (n) RETURN n.union AS u"}));
}

TEST(SplitQuery, IgnoresUnionInsideCallSubquery) {
    const std::string q = "CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x";
    EXPECT_EQ(splitQuery({q}), Strings({q}));
}

TEST(SplitQuery, SplitsOutsideButNotInsideCallSubquery) {
    EXPECT_EQ(splitQuery({"CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x UNION RETURN 3 AS x"}),
              Strings({"CALL { RETURN 1 AS x UNION RETURN 2 AS x } RETURN x", "RETURN 3 AS x"}));
}

TEST(SplitQuery, IgnoresUnionInComments) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x // UNION here\nUNION /* UNION */ RETURN 2 AS x"}),
              Strings({"RETURN 1 AS x // UNION here", "/* UNION */ RETURN 2 AS x"}));
}

TEST(SplitQuery, UnterminatedStringDoesNotCrash) {
    EXPECT_EQ(splitQuery({"RETURN 'oops UNION"}), Strings({"RETURN 'oops UNION"}));
}

TEST(SplitQuery, TrimsWhitespaceAndDropsEmptyBranches) {
    EXPECT_EQ(splitQuery({"  UNION RETURN 1 AS x  UNION  "}), Strings({"RETURN 1 AS x"}));
}

TEST(SplitQuery, EmptyInput) {
    EXPECT_TRUE(splitQuery({}).empty());
    EXPECT_TRUE(splitQuery({""}).empty());
    EXPECT_TRUE(splitQuery({"   \n\t"}).empty());
}

TEST(SplitQuery, HandlesMultipleStatementsInOrder) {
    EXPECT_EQ(splitQuery({"RETURN 1 AS x UNION RETURN 2 AS x", "MATCH (p) RETURN p"}),
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
        {"statement": "CREATE (n:Person {name: 'Alice'}) RETURN n AS person UNION CREATE (m:Person {name: \"Bob\"}) RETURN m AS person"}
    ]})");
    for (const auto& subquery : splitQuery(extractStatements(json_query))) {
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
