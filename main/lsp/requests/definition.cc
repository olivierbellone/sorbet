#include "core/lsp/QueryResponse.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {
void LSPLoop::addLocIfExists(rapidjson::Value &result, core::Loc loc) {
    if (loc.file().exists()) {
        result.PushBack(loc2Location(loc), alloc);
    }
}

void LSPLoop::handleTextDocumentDefinition(rapidjson::Value &result, rapidjson::Document &d) {
    prodCategoryCounterInc("lsp.requests.processed", "textDocument.definition");
    result.SetArray();
    if (auto run = setupLSPQueryByLoc(d, LSPMethod::TextDocumentDefinition(), true)) {
        auto &queryResponses = run->responses;
        if (!queryResponses.empty()) {
            auto resp = move(queryResponses[0]);

            if (resp->kind == core::QueryResponse::Kind::IDENT) {
                for (auto &originLoc : resp->retType.origins) {
                    addLocIfExists(result, originLoc);
                }
            } else if (resp->kind == core::QueryResponse::Kind::DEFINITION) {
                result.PushBack(loc2Location(resp->termLoc), alloc);
            } else {
                for (auto &component : resp->dispatchComponents) {
                    if (component.method.exists()) {
                        addLocIfExists(result, component.method.data(*finalGs)->loc());
                    }
                }
            }
        }
    }
    sendResult(d, result);
}

} // namespace sorbet::realmain::lsp
