#include "gtest/gtest.h"
// ^ Include first because it violates linting rules.

#include "test/lsp_test_helpers.h"
#include "test/position_assertions.h"
#include <regex>

using namespace std;

namespace sorbet::test {
// Matches '    #    ^^^^^ label: dafhdsjfkhdsljkfh*&#&*%'
// and '    # label: foobar'.
const regex rangeAssertionRegex("(#[ ]*)(\\^*)[ ]*([a-zA-Z]+):[ ]+(.*)$");

const regex whitespaceRegex("^[ ]*$");

// Maps assertion comment names to their constructors.
const UnorderedMap<string, function<shared_ptr<RangeAssertion>(string_view, unique_ptr<Range> &, int, string_view)>>
    assertionConstructors = {
        {"error", ErrorAssertion::make},
        {"usage", UsageAssertion::make},
        {"def", DefAssertion::make},
};

// Ignore any comments that have these labels (e.g. `# typed: true`).
const UnorderedSet<string> ignoredAssertionLabels = {"typed", "TODO", "linearization"};

// Compares the two positions. Returns -1 if `a` comes before `b`, 1 if `b` comes before `a`, and 0 if they are
// equivalent.
int positionComparison(const unique_ptr<Position> &a, const unique_ptr<Position> &b) {
    if (a->line < b->line) {
        return -1; // Less than
    } else if (a->line > b->line) {
        return 1;
    } else {
        // Same line
        if (a->character < b->character) {
            return -1;
        } else if (a->character > b->character) {
            return 1;
        } else {
            // Same position.
            return 0;
        }
    }
}

int rangeComparison(const unique_ptr<Range> &a, const unique_ptr<Range> &b) {
    int startPosCmp = positionComparison(a->start, b->start);
    if (startPosCmp != 0) {
        return startPosCmp;
    }
    // Whichever range ends earlier comes first.
    return positionComparison(a->end, b->end);
}

/** Returns true if `b` is a subset of `a`. Only works on single-line ranges. Assumes ranges are well-formed (start <=
 * end) */
bool rangeIsSubset(const unique_ptr<Range> &a, const unique_ptr<Range> &b) {
    if (a->start->line != a->end->line || b->start->line != b->end->line || a->start->line != b->start->line) {
        return false;
    }

    // One-liners on same line.
    return b->start->character >= a->start->character && b->end->character <= a->end->character;
}

int errorComparison(string_view aFilename, const unique_ptr<Range> &a, string_view aMessage, string_view bFilename,
                    const unique_ptr<Range> &b, string_view bMessage) {
    int filecmp = aFilename.compare(bFilename);
    if (filecmp != 0) {
        return filecmp;
    }
    int rangecmp = rangeComparison(a, b);
    if (rangecmp != 0) {
        return rangecmp;
    }
    return aMessage.compare(bMessage);
}

string prettyPrintRangeComment(string_view sourceLine, const unique_ptr<Range> &range, string_view comment) {
    int numLeadingSpaces = range->start->character;
    if (numLeadingSpaces < 0) {
        ADD_FAILURE() << fmt::format("Invalid range: {} < 0", range->start->character);
        return "";
    }
    string sourceLineNumber = fmt::format("{}", range->start->line + 1);
    EXPECT_EQ(range->start->line, range->end->line) << "Multi-line ranges are not supported at this time.";
    if (range->start->line != range->end->line) {
        return string(comment);
    }

    int numCarets = range->end->character - range->start->character;
    if (numCarets == RangeAssertion::END_OF_LINE_POS) {
        // Caret the entire line.
        numCarets = sourceLine.length();
    }

    return fmt::format("{} {}\n {}{} {}", sourceLineNumber, sourceLine,
                       string(numLeadingSpaces + sourceLineNumber.length(), ' '), string(numCarets, '^'), comment);
}

string_view getLine(const UnorderedMap<string, std::shared_ptr<core::File>> &sourceFileContents, string_view uriPrefix,
                    const unique_ptr<Location> &loc) {
    auto filename = uriToFilePath(uriPrefix, loc->uri);
    auto foundFile = sourceFileContents.find(filename);
    EXPECT_NE(sourceFileContents.end(), foundFile) << fmt::format("Unable to find file `{}`", filename);
    auto &file = foundFile->second;
    return file->getLine(loc->range->start->line + 1);
}

string filePathToUri(string_view prefixUrl, string_view filePath) {
    return fmt::format("{}/{}", prefixUrl, filePath);
}

string uriToFilePath(string_view prefixUrl, string_view uri) {
    if (uri.substr(0, prefixUrl.length()) != prefixUrl) {
        ADD_FAILURE() << fmt::format(
            "Unrecognized URI: `{}` is not contained in root URI `{}`, and thus does not correspond to a test file.",
            uri, prefixUrl);
        return "";
    }
    return string(uri.substr(prefixUrl.length() + 1));
}

RangeAssertion::RangeAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine)
    : filename(filename), range(move(range)), assertionLine(assertionLine) {}

int RangeAssertion::compare(string_view otherFilename, const unique_ptr<Range> &otherRange) {
    int filenamecmp = filename.compare(otherFilename);
    if (filenamecmp != 0) {
        return filenamecmp;
    }
    if (range->end->character == RangeAssertion::END_OF_LINE_POS) {
        // This assertion matches the whole line.
        // (Will match diagnostics that span multiple lines for parity with existing test logic.)
        int targetLine = range->start->line;
        if (targetLine >= otherRange->start->line && targetLine <= otherRange->end->line) {
            return 0;
        } else if (targetLine > otherRange->start->line) {
            return 1;
        } else {
            return -1;
        }
    }
    return rangeComparison(range, otherRange);
}

ErrorAssertion::ErrorAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view message)
    : RangeAssertion(filename, range, assertionLine), message(message) {}

shared_ptr<ErrorAssertion> ErrorAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                string_view assertionContents) {
    return make_shared<ErrorAssertion>(filename, range, assertionLine, assertionContents);
}

string ErrorAssertion::toString() {
    return fmt::format("error: {}", message);
}

void ErrorAssertion::check(const unique_ptr<Diagnostic> &diagnostic, const string_view sourceLine) {
    // The error message must contain `message`.
    if (diagnostic->message.find(message) == string::npos) {
        ADD_FAILURE_AT(filename.c_str(), range->start->line + 1) << fmt::format(
            "Expected error of form:\n{}\nFound error:\n{}", prettyPrintRangeComment(sourceLine, range, toString()),
            prettyPrintRangeComment(sourceLine, diagnostic->range, fmt::format("error: {}", diagnostic->message)));
    }
}

unique_ptr<Range> RangeAssertion::makeRange(int sourceLine, int startChar, int endChar) {
    auto rv = make_unique<Range>();
    rv->start = make_unique<Position>();
    rv->end = make_unique<Position>();
    rv->start->line = sourceLine;
    rv->start->character = startChar;
    rv->end->line = sourceLine;
    rv->end->character = endChar;
    return rv;
}

vector<shared_ptr<ErrorAssertion>>
RangeAssertion::getErrorAssertions(const vector<shared_ptr<RangeAssertion>> &assertions) {
    vector<shared_ptr<ErrorAssertion>> rv;
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<ErrorAssertion>(assertion)) {
            rv.push_back(assertionOfType);
        }
    }
    return rv;
}

vector<shared_ptr<LSPRequestResponseAssertion>>
RangeAssertion::getRequestResponseAssertions(const vector<shared_ptr<RangeAssertion>> &assertions) {
    vector<shared_ptr<LSPRequestResponseAssertion>> rv;
    for (auto assertion : assertions) {
        if (auto assertionOfType = dynamic_pointer_cast<LSPRequestResponseAssertion>(assertion)) {
            rv.push_back(assertionOfType);
        }
    }
    return rv;
}

vector<shared_ptr<RangeAssertion>> parseAssertionsForFile(const shared_ptr<core::File> &file) {
    vector<shared_ptr<RangeAssertion>> assertions;

    int nextChar = 0;
    // 'line' is from line linenum
    int lineNum = 0;
    // The last non-comment-assertion line that we've encountered.
    // When we encounter a comment assertion, it will refer to this
    // line.
    int lastSourceLineNum = 0;

    auto source = file->source();
    auto filename = string(file->path().begin(), file->path().end());
    auto &lineBreaks = file->line_breaks();

    for (auto lineBreak : lineBreaks) {
        // Ignore first line break entry.
        if (lineBreak == -1) {
            continue;
        }
        string_view lineView = source.substr(nextChar, lineBreak - nextChar);
        string line = string(lineView.begin(), lineView.end());
        nextChar = lineBreak + 1;

        // Groups: Line up until first caret, carets, assertion type, assertion contents.
        smatch matches;
        if (regex_search(line, matches, rangeAssertionRegex)) {
            int numCarets = matches[2].str().size();
            auto textBeforeComment = matches.prefix().str();
            bool lineHasCode = !regex_match(textBeforeComment, whitespaceRegex);
            if (numCarets != 0) {
                // Position assertion assertions.
                if (lineNum == 0) {
                    ADD_FAILURE_AT(filename.c_str(), lineNum + 1) << fmt::format(
                        "Invalid assertion comment found on line 1, before any code:\n{}\nAssertion comments that "
                        "point to "
                        "specific character ranges with carets (^) should come after the code they point to.",
                        line);
                    // Ignore erroneous comment.
                    continue;
                }
            }

            if (numCarets == 0 && lineHasCode) {
                // Line-based assertion comment is on a line w/ code, meaning
                // the assertion is for that line.
                lastSourceLineNum = lineNum;
            }

            unique_ptr<Range> range;
            if (numCarets > 0) {
                int caretBeginPos = textBeforeComment.size() + matches[1].str().size();
                int caretEndPos = caretBeginPos + numCarets;
                range = RangeAssertion::makeRange(lastSourceLineNum, caretBeginPos, caretEndPos);
            } else {
                range = RangeAssertion::makeRange(lastSourceLineNum);
            }

            if (numCarets != 0 && lineHasCode) {
                // Character-based assertion comment is on line w/ code, so
                // next line could point to code on this line.
                lastSourceLineNum = lineNum;
            }

            string assertionType = matches[3].str();
            string assertionContents = matches[4].str();

            const auto &findConstructor = assertionConstructors.find(assertionType);
            if (findConstructor != assertionConstructors.end()) {
                assertions.push_back(findConstructor->second(filename, range, lineNum, assertionContents));
            } else if (ignoredAssertionLabels.find(assertionType) == ignoredAssertionLabels.end()) {
                ADD_FAILURE_AT(filename.c_str(), lineNum + 1)
                    << fmt::format("Found unrecognized assertion of type `{}`. Expected one of {{{}}}.\nIf this is a "
                                   "regular comment that just happens to be formatted like an assertion comment, you "
                                   "can add the label to `ignoredAssertionLabels`.",
                                   assertionType,
                                   fmt::map_join(assertionConstructors.begin(), assertionConstructors.end(), ", ",
                                                 [](const auto &entry) -> string { return entry.first; }));
            }
        } else {
            lastSourceLineNum = lineNum;
        }
        lineNum += 1;
    }

    // Associate usage/def assertions with one another.
    // symbol => definition assertion
    UnorderedMap<string, shared_ptr<DefAssertion>> defAssertions;

    // Pass 1: Find def assertions, insert into map.
    for (auto &assertion : assertions) {
        if (auto defAssertion = dynamic_pointer_cast<DefAssertion>(assertion)) {
            {
                auto found = defAssertions.find(defAssertion->symbol);
                if (found != defAssertions.end()) {
                    auto &existingDefAssertion = found->second;
                    auto errorMessage = fmt::format(
                        "Found multiple def comments for label `{}`.\nPlease use unique labels for definition "
                        "assertions. Note that these labels do not need to match the pointed-to identifiers.\nFor "
                        "example, the following is completely valid:\n foo = 3\n#^^^ def: bar",
                        defAssertion->symbol);
                    ADD_FAILURE_AT(filename.c_str(), existingDefAssertion->assertionLine + 1) << errorMessage;
                    ADD_FAILURE_AT(filename.c_str(), defAssertion->assertionLine + 1) << errorMessage;
                    // Ignore duplicate symbol.
                    continue;
                }
            }
            defAssertions[defAssertion->symbol] = defAssertion;
        }
    }

    // Pass 2: Find usage assertions, associate with def assertion found with map.
    for (auto &assertion : assertions) {
        if (auto usageAssertion = dynamic_pointer_cast<UsageAssertion>(assertion)) {
            auto it = defAssertions.find(usageAssertion->symbol);
            if (it == defAssertions.end()) {
                ADD_FAILURE_AT(filename.c_str(), usageAssertion->assertionLine + 1) << fmt::format(
                    "Found usage comment for label {0} without matching def comment. Please add a `# ^^ def: {0}` "
                    "assertion that points to the definition of the pointed-to thing being used.",
                    usageAssertion->symbol);
                // Ignore invalid usage assertion.
                continue;
            }
            auto defAssertion = it->second;
            defAssertion->usages.push_back(usageAssertion);
            usageAssertion->def = defAssertion;
        }
    }

    return assertions;
}

vector<shared_ptr<RangeAssertion>>
RangeAssertion::parseAssertions(const UnorderedMap<string, shared_ptr<core::File>> filesAndContents) {
    vector<shared_ptr<RangeAssertion>> assertions;
    for (auto &fileAndContents : filesAndContents) {
        auto fileAssertions = parseAssertionsForFile(fileAndContents.second);
        assertions.insert(assertions.end(), make_move_iterator(fileAssertions.begin()),
                          make_move_iterator(fileAssertions.end()));
    }

    // Sort assertions in (filename, range, message) order
    fast_sort(assertions, [](const shared_ptr<RangeAssertion> &a, const shared_ptr<RangeAssertion> &b) -> bool {
        return errorComparison(a->filename, a->range, a->toString(), b->filename, b->range, b->toString()) == -1;
    });

    return assertions;
}

unique_ptr<Location> RangeAssertion::getLocation(string_view uriPrefix) {
    unique_ptr<Location> loc = make_unique<Location>();
    loc->range = make_unique<Range>();
    loc->range->start = make_unique<Position>();
    loc->range->start->line = range->start->line;
    loc->range->start->character = range->start->character;
    loc->range->end = make_unique<Position>();
    loc->range->end->line = range->end->line;
    loc->range->end->character = range->end->character;
    loc->uri = filePathToUri(uriPrefix, filename);
    return loc;
}

LSPRequestResponseAssertion::LSPRequestResponseAssertion(string_view filename, unique_ptr<Range> &range,
                                                         int assertionLine)
    : RangeAssertion(filename, range, assertionLine) {}

DefAssertion::DefAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view symbol)
    : LSPRequestResponseAssertion(filename, range, assertionLine), symbol(symbol) {}

shared_ptr<DefAssertion> DefAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                            string_view assertionContents) {
    return make_shared<DefAssertion>(filename, range, assertionLine, assertionContents);
}

vector<unique_ptr<Location>> extractLocations(const unique_ptr<JSONDocument<int>> &doc,
                                              const unique_ptr<rapidjson::Value> &obj) {
    vector<unique_ptr<Location>> locations;
    if (obj->IsArray()) {
        for (auto &element : obj->GetArray()) {
            locations.push_back(
                Location::fromJSONValue(doc->memoryOwner->GetAllocator(), element, "ResponseMessage.result"));
        }
    } else if (obj->IsObject()) {
        locations.push_back(
            Location::fromJSONValue(doc->memoryOwner->GetAllocator(), *obj.get(), "ResponseMessage.result"));
    }
    return locations;
}

void DefAssertion::checkDefinition(const Expectations &expectations, LSPTest &test, string_view uriPrefix,
                                   unique_ptr<JSONDocument<int>> &doc, const unique_ptr<Location> &loc, int character,
                                   int id, string_view defSourceLine) {
    const int line = loc->range->start->line;
    auto locSourceLine = getLine(expectations.sourceFileContents, uriPrefix, loc);
    string locFilename = uriToFilePath(uriPrefix, loc->uri);
    string defUri = filePathToUri(uriPrefix, filename);

    auto textDocumentPositionParams = make_unique<TextDocumentPositionParams>();
    textDocumentPositionParams->textDocument = make_unique<TextDocumentIdentifier>();
    textDocumentPositionParams->textDocument->uri = loc->uri;
    textDocumentPositionParams->position = make_unique<Position>();
    textDocumentPositionParams->position->line = line;
    textDocumentPositionParams->position->character = character;

    unique_ptr<JSONBaseType> cast = move(textDocumentPositionParams);
    auto responses = test.getLSPResponsesFor(makeRequestMessage(doc, "textDocument/definition", id, cast));
    if (responses.size() != 1) {
        EXPECT_EQ(1, responses.size()) << "Unexpected number of responses to a `textDocument/definition` request.";
        return;
    }

    if (auto maybeDoc = assertResponseMessage(id, responses.at(0))) {
        auto &respMsg = (*maybeDoc)->root;
        ASSERT_TRUE(respMsg->result.has_value());
        ASSERT_FALSE(respMsg->error.has_value());

        auto &result = *(respMsg->result);
        vector<unique_ptr<Location>> locations = extractLocations(doc, result);

        if (locations.size() == 0) {
            ADD_FAILURE_AT(locFilename.c_str(), line + 1) << fmt::format(
                "Sorbet did not find a definition for location that references symbol `{}`.\nExpected definition "
                "of:\n{}\nTo be:\n{}",
                symbol, prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(defSourceLine, range, ""));
            return;
        } else if (locations.size() > 1) {
            ADD_FAILURE_AT(locFilename.c_str(), line + 1) << fmt::format(
                "Sorbet unexpectedly returned multiple locations for definition of symbol `{}`.\nFor "
                "location:\n{}\nSorbet returned the following definition locations:\n{}",
                symbol, prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                fmt::map_join(locations, "\n", [&expectations, &uriPrefix](const auto &arg) -> string {
                    return prettyPrintRangeComment(getLine(expectations.sourceFileContents, uriPrefix, arg), arg->range,
                                                   "");
                }));
            return;
        }

        auto &location = locations.at(0);
        // Note: Sorbet will point to the *statement* that defines the symbol, not just the symbol.
        // For example, it'll point to "class Foo" instead of just "Foo". Thus, we just check that "Foo"
        // is in the range reported.
        if (location->uri != defUri || !rangeIsSubset(location->range, range)) {
            string foundLocationString = "null";
            if (location != nullptr) {
                foundLocationString = prettyPrintRangeComment(
                    getLine(expectations.sourceFileContents, uriPrefix, location), location->range, "");
            }

            ADD_FAILURE_AT(filename.c_str(), line + 1)
                << fmt::format("Sorbet did not return the expected definition for location. Expected "
                               "definition of:\n{}\nTo be:\n{}\nBut was:\n{}",
                               prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                               prettyPrintRangeComment(defSourceLine, range, ""), foundLocationString);
        }
    }
}

void DefAssertion::checkReferences(const Expectations &expectations, LSPTest &test, string_view uriPrefix,
                                   unique_ptr<JSONDocument<int>> &doc, const vector<unique_ptr<Location>> &allLocs,
                                   const unique_ptr<Location> &loc, int character, int id, string_view defSourceLine) {
    const int line = loc->range->start->line;
    auto locSourceLine = getLine(expectations.sourceFileContents, uriPrefix, loc);
    string locFilename = uriToFilePath(uriPrefix, loc->uri);
    string defUri = filePathToUri(uriPrefix, filename);

    auto referenceParams = make_unique<ReferenceParams>();
    referenceParams->textDocument = make_unique<TextDocumentIdentifier>();
    referenceParams->textDocument->uri = loc->uri;
    referenceParams->position = make_unique<Position>();
    referenceParams->position->line = line;
    referenceParams->position->character = character;
    referenceParams->context = make_unique<ReferenceContext>();
    // TODO: Try with this false, too.
    referenceParams->context->includeDeclaration = true;

    unique_ptr<JSONBaseType> cast = move(referenceParams);
    auto responses = test.getLSPResponsesFor(makeRequestMessage(doc, "textDocument/references", id, cast));
    if (responses.size() != 1) {
        EXPECT_EQ(1, responses.size()) << "Unexpected number of responses to a `textDocument/references` request.";
        return;
    }

    if (auto maybeDoc = assertResponseMessage(id, responses.at(0))) {
        auto &respMsg = (*maybeDoc)->root;
        ASSERT_TRUE(respMsg->result.has_value());
        ASSERT_FALSE(respMsg->error.has_value());
        auto &result = *(respMsg->result);

        vector<unique_ptr<Location>> locations = extractLocations(doc, result);
        fast_sort(locations, [&](const unique_ptr<Location> &a, const unique_ptr<Location> &b) -> bool {
            return errorComparison(a->uri, a->range, "", b->uri, b->range, "");
        });

        auto expectedLocationsIt = allLocs.begin();
        auto actualLocationsIt = locations.begin();
        while (expectedLocationsIt != allLocs.end() && actualLocationsIt != locations.end()) {
            auto &expectedLocation = *expectedLocationsIt;
            auto &actualLocation = *actualLocationsIt;

            // If true, the expectedLocation is a subset of the actualLocation
            if (actualLocation->uri == expectedLocation->uri &&
                rangeIsSubset(actualLocation->range, expectedLocation->range)) {
                // Assertion passes. Consume both.
                actualLocationsIt++;
                expectedLocationsIt++;
            } else {
                switch (errorComparison(expectedLocation->uri, expectedLocation->range, "", actualLocation->uri,
                                        actualLocation->range, "")) {
                    case -1: {
                        // Expected location is *before* actual location.
                        auto expectedFilePath = uriToFilePath(uriPrefix, expectedLocation->uri);
                        ADD_FAILURE_AT(expectedFilePath.c_str(), expectedLocation->range->start->line + 1)
                            << fmt::format(
                                   "Sorbet did not report a reference to symbol `{}`.\nGiven symbol at:\n{}\nSorbet "
                                   "did not report reference at:\n{}",
                                   symbol,
                                   prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1),
                                                           ""),
                                   prettyPrintRangeComment(
                                       getLine(expectations.sourceFileContents, uriPrefix, expectedLocation),
                                       expectedLocation->range, ""));
                        expectedLocationsIt++;
                        break;
                    }
                    case 1: {
                        // Expected location is *after* actual location
                        auto actualFilePath = uriToFilePath(uriPrefix, actualLocation->uri);
                        ADD_FAILURE_AT(actualFilePath.c_str(), actualLocation->range->start->line + 1) << fmt::format(
                            "Sorbet reported unexpected reference to symbom `{}`.\nGiven symbol "
                            "at:\n{}\nSorbet reported an unexpected reference at:\n{}",
                            symbol,
                            prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                            prettyPrintRangeComment(getLine(expectations.sourceFileContents, uriPrefix, actualLocation),
                                                    actualLocation->range, ""));
                        actualLocationsIt++;
                        break;
                    }
                    default:
                        // Should never happen.
                        ADD_FAILURE()
                            << "Error in test runner: identical locations weren't reported as subsets of one another.";
                        break;
                }
            }
        }

        while (expectedLocationsIt != allLocs.end()) {
            auto &expectedLocation = *expectedLocationsIt;
            auto expectedFilePath = uriToFilePath(uriPrefix, expectedLocation->uri);
            ADD_FAILURE_AT(expectedFilePath.c_str(), expectedLocation->range->start->line + 1) << fmt::format(
                "Sorbet did not report a reference to symbol `{}`.\nGiven symbol at:\n{}\nSorbet "
                "did not report reference at:\n{}",
                symbol, prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(expectations.sourceFileContents, uriPrefix, expectedLocation),
                                        expectedLocation->range, ""));
            expectedLocationsIt++;
        }

        while (actualLocationsIt != locations.end()) {
            auto &actualLocation = *actualLocationsIt;
            auto actualFilePath = uriToFilePath(uriPrefix, actualLocation->uri);
            ADD_FAILURE_AT(actualFilePath.c_str(), actualLocation->range->start->line + 1) << fmt::format(
                "Sorbet reported unexpected reference to symbom `{}`.\nGiven symbol "
                "at:\n{}\nSorbet reported an unexpected reference at:\n{}",
                symbol, prettyPrintRangeComment(locSourceLine, makeRange(line, character, character + 1), ""),
                prettyPrintRangeComment(getLine(expectations.sourceFileContents, uriPrefix, actualLocation),
                                        actualLocation->range, ""));
            actualLocationsIt++;
        }
    }
}

void DefAssertion::check(const Expectations &expectations, LSPTest &test, unique_ptr<JSONDocument<int>> &doc,
                         string_view uriPrefix, int &nextId) {
    auto locationsToCheck = vector<unique_ptr<Location>>();
    locationsToCheck.push_back(getLocation(uriPrefix));
    auto defSourceLine = getLine(expectations.sourceFileContents, uriPrefix, locationsToCheck.at(0));

    for (auto &usage : usages) {
        locationsToCheck.push_back(usage->getLocation(uriPrefix));
    }

    // Canonicalize order for reference comparison.
    fast_sort(locationsToCheck, [&](const unique_ptr<Location> &a, const unique_ptr<Location> &b) -> bool {
        return errorComparison(a->uri, a->range, "", b->uri, b->range, "");
    });

    for (auto &location : locationsToCheck) {
        auto &locRange = location->range;
        // Should never happen -- there's no way to construct them.
        EXPECT_EQ(locRange->start->line, locRange->end->line)
            << "Multi-line ranges are not supported for position assertions.";

        // Every character in range should work as a source location for a definition or reference request, but we'll
        // just check the first character to avoid blowing up test failures.
        checkDefinition(expectations, test, uriPrefix, doc, location, locRange->start->character, nextId++,
                        defSourceLine);
        checkReferences(expectations, test, uriPrefix, doc, locationsToCheck, location, locRange->start->character,
                        nextId++, defSourceLine);
    }
}

string DefAssertion::toString() {
    return fmt::format("def: {}", symbol);
}

UsageAssertion::UsageAssertion(string_view filename, unique_ptr<Range> &range, int assertionLine, string_view symbol)
    : RangeAssertion(filename, range, assertionLine), symbol(symbol) {}

shared_ptr<UsageAssertion> UsageAssertion::make(string_view filename, unique_ptr<Range> &range, int assertionLine,
                                                string_view assertionContents) {
    return make_shared<UsageAssertion>(filename, range, assertionLine, assertionContents);
}

string UsageAssertion::toString() {
    return fmt::format("usage: {}", symbol);
}

} // namespace sorbet::test
