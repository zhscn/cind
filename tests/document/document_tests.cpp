#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "document/document.hpp"
#include "document/line_index.hpp"

#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cind;

namespace {

// Reference application of a normalized edit list (old coordinates,
// ascending, non-overlapping): apply back to front.
std::string apply_normalized(std::string text, const std::vector<TextEdit>& edits) {
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        text.replace(it->old_range.start.value, it->old_range.length(), it->new_text);
    }
    return text;
}

void check_normalized(const std::vector<TextEdit>& edits) {
    for (std::size_t i = 0; i < edits.size(); ++i) {
        CHECK(edits[i].old_range.start.value <= edits[i].old_range.end.value);
        if (i > 0) {
            CHECK(edits[i - 1].old_range.end.value <= edits[i].old_range.start.value);
        }
    }
}

// Index is LineIndex or Text (same line-query API).
template <typename Index>
void check_line_index(std::string_view text, const Index& index) {
    std::vector<std::uint32_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    REQUIRE(index.line_count() == starts.size());
    for (std::size_t line = 0; line < starts.size(); ++line) {
        CHECK(index.line_start(static_cast<std::uint32_t>(line)).value == starts[line]);
    }
    std::uint32_t line = 0;
    for (std::uint32_t off = 0; off <= text.size(); ++off) {
        while (line + 1 < starts.size() && starts[line + 1] <= off) {
            ++line;
        }
        LinePosition pos = index.position(TextOffset{off});
        CHECK(pos.line == line);
        CHECK(pos.byte_column == off - starts[line]);
        CHECK(index.offset(pos).value == off);
    }
}

} // namespace

TEST_CASE("empty document basics") {
    Document doc("");
    auto snap = doc.snapshot();
    CHECK(snap.revision() == 0);
    CHECK(snap.content() == "");
    CHECK(snap.size_bytes() == 0);
    CHECK(snap.content().line_count() == 1);
}

TEST_CASE("single edits") {
    Document doc("hello world");

    SUBCASE("insert") {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{5}, ",");
        auto result = tx.commit();
        CHECK(doc.snapshot().content() == "hello, world");
        CHECK(result.change.old_revision == 0);
        CHECK(result.change.new_revision == 1);
        REQUIRE(result.change.edits.size() == 1);
        CHECK(result.change.edits[0].old_range == make_range(5, 5));
        CHECK(result.change.edits[0].new_text == ",");
        CHECK(result.change.affected_old_range == make_range(5, 5));
        CHECK(result.change.affected_new_range == make_range(5, 6));
    }
    SUBCASE("erase") {
        auto tx = doc.begin_transaction();
        tx.erase(make_range(5, 11));
        tx.commit();
        CHECK(doc.snapshot().content() == "hello");
    }
    SUBCASE("replace") {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(6, 11), "there");
        tx.commit();
        CHECK(doc.snapshot().content() == "hello there");
    }
}

TEST_CASE("snapshots are immutable across commits") {
    Document doc("abc");
    auto before = doc.snapshot();
    {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(0, 3), "xyz");
        tx.commit();
    }
    CHECK(before.content() == "abc");
    CHECK(before.revision() == 0);
    CHECK(doc.snapshot().content() == "xyz");
    CHECK(doc.snapshot().revision() == 1);
}

TEST_CASE("multi-edit transaction produces one normalized change") {
    Document doc("void f() {}");
    // Enter between braces: caret at 10, insert "\n" then "    " — the
    // canonical newline-and-indent shape.
    AnchorId caret = doc.create_anchor(TextOffset{10}, AnchorAffinity::AfterInsertion);
    auto tx = doc.begin_transaction();
    tx.insert(tx.anchor_offset(caret), "\n");
    CHECK(tx.anchor_offset(caret).value == 11);
    tx.insert(tx.anchor_offset(caret), "    ");
    CHECK(tx.anchor_offset(caret).value == 15);
    auto result = tx.commit();

    CHECK(doc.snapshot().content() == "void f() {\n    }");
    CHECK(doc.anchor_offset(caret).value == 15);
    // Adjacent inserts merged into a single edit.
    REQUIRE(result.change.edits.size() == 1);
    CHECK(result.change.edits[0].old_range == make_range(10, 10));
    CHECK(result.change.edits[0].new_text == "\n    ");

    // One undo unit reverts the whole command.
    REQUIRE(doc.undo().has_value());
    CHECK(doc.snapshot().content() == "void f() {}");
    CHECK(doc.anchor_offset(caret).value == 10);
}

TEST_CASE("edit folding keeps the list normalized") {
    Document doc("0123456789");

    SUBCASE("disjoint edits arriving out of order") {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{8}, "B");
        tx.insert(TextOffset{2}, "A");
        auto result = tx.commit();
        CHECK(doc.snapshot().content() == "01A234567B89");
        REQUIRE(result.change.edits.size() == 2);
        check_normalized(result.change.edits);
        CHECK(apply_normalized("0123456789", result.change.edits) == "01A234567B89");
    }
    SUBCASE("replace spanning an earlier insertion merges") {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{5}, "XX"); // "01234XX56789"
        tx.replace(make_range(4, 9), "-"); // spans "4XX56"
        auto result = tx.commit();
        CHECK(doc.snapshot().content() == "0123-789");
        REQUIRE(result.change.edits.size() == 1);
        CHECK(apply_normalized("0123456789", result.change.edits) == "0123-789");
    }
    SUBCASE("erase across two earlier edits merges all three") {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{2}, "A");  // "01A23456789"
        tx.insert(TextOffset{9}, "B");  // "01A234567B89"
        tx.erase(make_range(1, 11));    // spans both, keeps "0" and the final "9"
        auto result = tx.commit();
        CHECK(doc.snapshot().content() == "09");
        REQUIRE(result.change.edits.size() == 1);
        CHECK(apply_normalized("0123456789", result.change.edits) == "09");
    }
}

TEST_CASE("abort leaves the document untouched") {
    Document doc("stable");
    AnchorId a = doc.create_anchor(TextOffset{3}, AnchorAffinity::BeforeInsertion);
    {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{0}, "unstable ");
        CHECK(tx.anchor_offset(a).value == 12);
        tx.abort();
    }
    CHECK(doc.snapshot().content() == "stable");
    CHECK(doc.revision() == 0);
    CHECK(doc.anchor_offset(a).value == 3);

    SUBCASE("destructor aborts an uncommitted transaction") {
        {
            auto tx = doc.begin_transaction();
            tx.insert(TextOffset{0}, "x");
        }
        CHECK(doc.snapshot().content() == "stable");
        auto tx = doc.begin_transaction(); // no "already active" throw
        tx.abort();
    }
}

TEST_CASE("only one active transaction") {
    Document doc("x");
    auto tx = doc.begin_transaction();
    CHECK_THROWS_AS(doc.begin_transaction(), std::logic_error);
    CHECK_THROWS_AS(doc.undo(), std::logic_error);
    CHECK_THROWS_AS(doc.create_anchor(TextOffset{0}, AnchorAffinity::BeforeInsertion),
                    std::logic_error);
    tx.abort();
    CHECK_THROWS_AS(tx.commit(), std::logic_error);
}

TEST_CASE("edit validation") {
    Document doc("abc");
    auto tx = doc.begin_transaction();
    CHECK_THROWS_AS(tx.insert(TextOffset{4}, "x"), std::out_of_range);
    CHECK_THROWS_AS(tx.erase(make_range(2, 1)), std::out_of_range);
    CHECK_THROWS_AS(tx.insert(TextOffset{0}, "a\r\nb"), std::invalid_argument);
    tx.insert(TextOffset{3}, "!");
    tx.commit();
    CHECK(doc.snapshot().content() == "abc!");
}

TEST_CASE("empty commit does not bump the revision") {
    Document doc("abc");
    auto tx = doc.begin_transaction();
    auto result = tx.commit();
    CHECK(result.change.old_revision == result.change.new_revision);
    CHECK(doc.revision() == 0);
    CHECK(!doc.can_undo());
}

TEST_CASE("speculative snapshot") {
    Document doc("ab");
    auto tx = doc.begin_transaction();
    CHECK(tx.speculative_snapshot().revision() == 0); // no edits yet
    tx.insert(TextOffset{1}, "\n");
    auto spec = tx.speculative_snapshot();
    CHECK(spec.revision() == 1);
    CHECK(spec.content() == "a\nb");
    CHECK(spec.content().line_count() == 2);
    tx.abort();
    CHECK(doc.revision() == 0);
    CHECK(spec.content() == "a\nb"); // snapshot outlives the transaction
}

TEST_CASE("undo and redo") {
    Document doc("v0");
    {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(0, 2), "v1");
        tx.commit();
    }
    {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{2}, "+x");
        tx.commit();
    }
    CHECK(doc.snapshot().content() == "v1+x");
    CHECK(doc.revision() == 2);

    REQUIRE(doc.undo().has_value());
    CHECK(doc.snapshot().content() == "v1");
    CHECK(doc.revision() == 3); // undo creates a new revision

    REQUIRE(doc.undo().has_value());
    CHECK(doc.snapshot().content() == "v0");
    CHECK(!doc.can_undo());
    CHECK(!doc.undo().has_value());

    REQUIRE(doc.redo().has_value());
    CHECK(doc.snapshot().content() == "v1");
    REQUIRE(doc.redo().has_value());
    CHECK(doc.snapshot().content() == "v1+x");
    CHECK(!doc.can_redo());

    // A commit after undo starts a fresh branch: no redo at its tip, but the
    // old branch stays reachable through the parent (see the tree tests).
    doc.undo();
    {
        auto tx = doc.begin_transaction();
        tx.insert(TextOffset{0}, "!");
        tx.commit();
    }
    CHECK(!doc.can_redo());
}

TEST_CASE("undo tree: branches, redo picks the newest, undo_to jumps") {
    Document doc("r");
    const UndoNodeId root = doc.undo_position();
    CHECK(root == 0);

    auto edit = [&](std::string_view insert) {
        auto tx = doc.begin_transaction();
        tx.insert(doc.snapshot().end_offset(), insert);
        tx.commit();
        return doc.undo_position();
    };

    const UndoNodeId a = edit("A");  // "rA"
    const UndoNodeId a2 = edit("2"); // "rA2"
    REQUIRE(doc.undo().has_value()); // back to "rA"
    REQUIRE(doc.undo().has_value()); // back to "r"
    const UndoNodeId b = edit("B");  // "rB" — second branch off the root
    CHECK(doc.undo_children(root).size() == 2);
    CHECK(doc.undo_parent(a) == root);
    CHECK(doc.undo_parent(b) == root);

    // Preview without switching.
    CHECK(doc.undo_node_text(a2) == "rA2");
    CHECK(doc.snapshot().content() == "rB");

    // redo from the root goes to the newest branch (B).
    REQUIRE(doc.undo().has_value());
    CHECK(doc.snapshot().content() == "r");
    CHECK(doc.can_redo());
    REQUIRE(doc.redo().has_value());
    CHECK(doc.snapshot().content() == "rB");
    CHECK(doc.undo_position() == b);

    // Jump across branches in one revision; the change replays cleanly.
    std::string before_text = doc.snapshot().content().to_string();
    const RevisionId rev = doc.revision();
    DocumentChange change = doc.undo_to(a2);
    CHECK(doc.snapshot().content() == "rA2");
    CHECK(doc.undo_position() == a2);
    CHECK(doc.revision() == rev + 1);
    check_normalized(change.edits);
    CHECK(apply_normalized(before_text, change.edits) == "rA2");

    // Jumping to the current node is a no-op change.
    DocumentChange same = doc.undo_to(a2);
    CHECK(same.edits.empty());
    CHECK(same.old_revision == same.new_revision);
    CHECK(doc.revision() == rev + 1);

    CHECK_THROWS_AS(doc.undo_to(doc.undo_node_count()), std::out_of_range);
}

TEST_CASE("undo tree: random walk matches per-node model") {
    std::mt19937 rng(424242);
    Document doc("seed\n");
    std::vector<std::string> node_text{doc.snapshot().content().to_string()};

    auto rand_int = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };

    for (int op = 0; op < 300; ++op) {
        switch (rand_int(0, 5)) {
        case 0:
        case 1: { // edit: new child node
            std::string cur = node_text[doc.undo_position()];
            std::uint32_t at = rand_int(0, static_cast<std::uint32_t>(cur.size()));
            std::string piece = "x" + std::to_string(op);
            auto tx = doc.begin_transaction();
            tx.insert(TextOffset{at}, piece);
            tx.commit();
            cur.insert(at, piece);
            REQUIRE(doc.undo_position() == node_text.size());
            node_text.push_back(std::move(cur));
            break;
        }
        case 2: doc.undo(); break;
        case 3: doc.redo(); break;
        default: { // undo_to a random known node
            const auto target = static_cast<UndoNodeId>(
                rand_int(0, static_cast<std::uint32_t>(node_text.size() - 1)));
            std::string before = doc.snapshot().content().to_string();
            DocumentChange change = doc.undo_to(target);
            check_normalized(change.edits);
            REQUIRE(apply_normalized(before, change.edits) == node_text[target]);
            break;
        }
        }
        REQUIRE(doc.undo_node_count() == node_text.size());
        REQUIRE(doc.snapshot().content() == node_text[doc.undo_position()]);
    }
}

TEST_CASE("line index") {
    check_line_index("", LineIndex(""));
    check_line_index("a", LineIndex("a"));
    check_line_index("a\n", LineIndex("a\n"));
    check_line_index("\n\n", LineIndex("\n\n"));
    check_line_index("ab\ncd\n\nef", LineIndex("ab\ncd\n\nef"));

    LineIndex idx("ab\ncd\n");
    CHECK(idx.line_count() == 3);
    CHECK(idx.line_range(0) == make_range(0, 3));
    CHECK(idx.line_content_range(0) == make_range(0, 2));
    CHECK(idx.line_range(2) == make_range(6, 6));
    CHECK_THROWS_AS(idx.line_start(3), std::out_of_range);
    CHECK_THROWS_AS(idx.position(TextOffset{7}), std::out_of_range);
    CHECK_THROWS_AS(idx.offset(LinePosition{0, 4}), std::out_of_range);
}

TEST_CASE("newline normalization in constructor") {
    Document doc("a\r\nb\rc\n");
    CHECK(doc.snapshot().content() == "a\nb\nc\n");
    CHECK(doc.snapshot().content().line_count() == 4);
}

TEST_CASE("anchor affinity at the insertion point") {
    Document doc("abc");
    AnchorId before = doc.create_anchor(TextOffset{1}, AnchorAffinity::BeforeInsertion);
    AnchorId after = doc.create_anchor(TextOffset{1}, AnchorAffinity::AfterInsertion);
    auto tx = doc.begin_transaction();
    tx.insert(TextOffset{1}, "XY");
    CHECK(tx.anchor_offset(before).value == 1);
    CHECK(tx.anchor_offset(after).value == 3);
    tx.commit();
    CHECK(doc.anchor_offset(before).value == 1);
    CHECK(doc.anchor_offset(after).value == 3);
}

TEST_CASE("anchor adjustment around replaces") {
    Document doc("0123456789");
    AnchorId a = doc.create_anchor(TextOffset{5}, AnchorAffinity::BeforeInsertion);

    SUBCASE("edit strictly before shifts") {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(0, 2), "zzz");
        CHECK(tx.anchor_offset(a).value == 6);
        tx.commit();
    }
    SUBCASE("edit strictly after does not move it") {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(7, 9), "");
        CHECK(tx.anchor_offset(a).value == 5);
        tx.commit();
    }
    SUBCASE("anchor inside a replaced range settles after the new text") {
        auto tx = doc.begin_transaction();
        tx.replace(make_range(4, 7), "AB");
        CHECK(tx.anchor_offset(a).value == 6); // 4 + len("AB")
        tx.commit();
    }
    SUBCASE("anchor at the start of an erased range stays") {
        auto tx = doc.begin_transaction();
        tx.erase(make_range(5, 8));
        CHECK(tx.anchor_offset(a).value == 5);
        tx.commit();
    }
    SUBCASE("anchor at the end of an erased range collapses to its start") {
        auto tx = doc.begin_transaction();
        tx.erase(make_range(2, 5));
        CHECK(tx.anchor_offset(a).value == 2);
        tx.commit();
    }
}

TEST_CASE("fuzz: random transactions keep every invariant") {
    std::mt19937 rng(20260713);
    auto rand_int = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };

    std::string model = "int main() {\n    return 0;\n}\n";
    Document doc(model);
    std::vector<std::string> history{model};
    AnchorId anchor = doc.create_anchor(TextOffset{5}, AnchorAffinity::AfterInsertion);

    static constexpr const char* kSnippets[] = {"",    "x",    "ab\n", "{\n}",
                                                "   ", "foo(", "\n\n", "namespace n {"};

    for (int round = 0; round < 300; ++round) {
        std::string before = model;
        auto tx = doc.begin_transaction();
        const std::uint32_t edit_count = rand_int(1, 4);
        for (std::uint32_t e = 0; e < edit_count; ++e) {
            const auto size = static_cast<std::uint32_t>(model.size());
            std::uint32_t a = rand_int(0, size);
            std::uint32_t b = rand_int(0, size);
            if (a > b) {
                std::swap(a, b);
            }
            if (b - a > 8) {
                b = a + 8;
            }
            const std::string_view snippet = kSnippets[rand_int(0, 7)];
            tx.replace(make_range(a, b), snippet);
            model.replace(a, b - a, snippet);
            REQUIRE(tx.current_text() == model);
        }
        auto result = tx.commit();
        history.push_back(model);

        REQUIRE(doc.snapshot().content() == model);
        check_normalized(result.change.edits);
        REQUIRE(apply_normalized(before, result.change.edits) == model);
        check_line_index(model, doc.snapshot().content());
        CHECK(doc.anchor_offset(anchor).value <= model.size());
    }

    for (std::size_t i = history.size(); i-- > 1;) {
        REQUIRE(doc.undo().has_value());
        REQUIRE(doc.snapshot().content() == history[i - 1]);
    }
    CHECK(!doc.can_undo());
    for (std::size_t i = 1; i < history.size(); ++i) {
        REQUIRE(doc.redo().has_value());
        REQUIRE(doc.snapshot().content() == history[i]);
    }
    CHECK(!doc.can_redo());

    // Revisions stayed strictly monotonic through all of it.
    CHECK(doc.revision() == 3 * (history.size() - 1));
}
