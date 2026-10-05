#include "tools/progen.h"

#include <random>
#include <sstream>
#include <vector>

namespace ember {

namespace {

enum class T { Int, Bool, Float, Str, IntArr, P, E, Fn };

const char* type_text(T t) {
    switch (t) {
        case T::Int: return "Int";
        case T::Bool: return "Bool";
        case T::Float: return "Float";
        case T::Str: return "String";
        case T::IntArr: return "[Int]";
        case T::P: return "P";
        case T::E: return "E";
        case T::Fn: return "fn(Int) -> Int";
    }
    return "?";
}

struct Var {
    std::string name;
    T type;
    bool mut;
};

struct FnSig {
    std::string name;
    int arity;  // all params Int, result Int
};

class Gen {
public:
    explicit Gen(uint64_t seed) : rng_(seed) {}

    std::string program() {
        o_ << "import strings;\nimport math;\n\n";
        o_ << "struct P { a: Int, b: Int }\n";
        o_ << "enum E { A(Int), B, C(Int, Int) }\n\n";
        // Globals.
        int ng = pick(0, 3);
        scopes_.emplace_back();
        for (int i = 0; i < ng; ++i) {
            std::string n = "g" + std::to_string(i);
            T t = chance(70) ? T::Int : T::Str;
            o_ << "var " << n << ": " << type_text(t) << " = " << expr(t, 2) << ";\n";
            scopes_.back().push_back(Var{n, t, true});
        }
        o_ << "\n";
        // A bounded recursive function.
        o_ << "fn rec(n: Int) -> Int {\n    if n <= 0 { return 1; }\n    return rec(n - 1) + n % 7;\n}\n\n";
        fns_.push_back(FnSig{"rec", 1});
        int nf = pick(0, 4);
        for (int i = 0; i < nf; ++i) function("f" + std::to_string(i));
        o_ << "fn main() {\n";
        in_fn_ = true;
        indent_ = 1;
        scopes_.emplace_back();
        int n = pick(3, 12);
        for (int i = 0; i < n; ++i) stmt(3);
        scopes_.pop_back();
        o_ << "}\n";
        return o_.str();
    }

private:
    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
    bool chance(int pct) { return pick(1, 100) <= pct; }

    void line(const std::string& s) { o_ << std::string(static_cast<size_t>(indent_) * 4, ' ') << s << "\n"; }

    std::string fresh(const char* prefix) { return std::string(prefix) + std::to_string(counter_++); }

    std::vector<const Var*> vars_of(T t, bool need_mut = false) {
        std::vector<const Var*> r;
        for (auto& sc : scopes_)
            for (auto& v : sc)
                if (v.type == t && (!need_mut || v.mut)) r.push_back(&v);
        return r;
    }

    void function(const std::string& name) {
        int arity = pick(0, 3);
        o_ << "fn " << name << "(";
        scopes_.emplace_back();
        for (int i = 0; i < arity; ++i) {
            if (i) o_ << ", ";
            std::string p = "p" + std::to_string(i);
            o_ << p << ": Int";
            scopes_.back().push_back(Var{p, T::Int, false});
        }
        o_ << ") -> Int {\n";
        indent_ = 1;
        bool saved = in_fn_;
        in_fn_ = true;
        int n = pick(0, 4);
        for (int i = 0; i < n; ++i) stmt(2);
        line("return " + expr(T::Int, 3) + ";");
        in_fn_ = saved;
        scopes_.pop_back();
        o_ << "}\n\n";
        fns_.push_back(FnSig{name, arity});
    }

    // ------------------------------------------------------------ statements
    void stmt(int depth) {
        int r = pick(0, depth <= 0 ? 6 : 13);
        switch (r) {
            case 0:
            case 1: {  // declaration
                T t = random_type();
                bool m = chance(60);
                std::string n = fresh("v");
                line(std::string(m ? "var " : "let ") + n + ": " + type_text(t) + " = " + expr(t, 3) + ";");
                scopes_.back().push_back(Var{n, t, m});
                break;
            }
            case 2:
            case 3: print_stmt(); break;
            case 4: assign_stmt(); break;
            case 5: {  // push / pop
                auto arrs = vars_of(T::IntArr);
                if (arrs.empty()) {
                    print_stmt();
                    break;
                }
                const Var* a = arrs[static_cast<size_t>(pick(0, static_cast<int>(arrs.size()) - 1))];
                // Never grow an array that an enclosing for-each iterates
                // (the loop re-reads its length and would never end).
                bool iterated = false;
                for (const auto& n : iterating_) iterated |= n == a->name;
                if (chance(75) && !iterated) line("push(" + a->name + ", " + expr(T::Int, 2) + ");");
                else line("print(pop(" + a->name + "));");
                break;
            }
            case 6: {  // field / element update
                auto ps = vars_of(T::P);
                if (!ps.empty() && chance(50)) {
                    const Var* p = ps[static_cast<size_t>(pick(0, static_cast<int>(ps.size()) - 1))];
                    std::string field = chance(50) ? ".a" : ".b";
                    std::string op = chance(50) ? " = " : " += ";
                    std::string rhs = expr(T::Int, 2);
                    line(p->name + field + op + rhs + ";");
                } else {
                    print_stmt();
                }
                break;
            }
            case 7:
            case 8: {  // if / else
                line("if " + expr(T::Bool, 3) + " {");
                block(depth - 1);
                if (chance(50)) {
                    line("} else {");
                    block(depth - 1);
                }
                line("}");
                break;
            }
            case 9: {  // for range
                std::string i = fresh("i");
                int lo = pick(-2, 3), hi = lo + pick(0, 5);
                line("for " + i + " in " + std::to_string(lo) + ".." + std::to_string(hi) + " {");
                ++loop_depth_;
                scopes_.emplace_back();
                scopes_.back().push_back(Var{i, T::Int, false});
                block_body(depth - 1);
                scopes_.pop_back();
                --loop_depth_;
                line("}");
                break;
            }
            case 10: {  // while with counter
                std::string c = fresh("w");
                line("var " + c + " = 0;");
                scopes_.back().push_back(Var{c, T::Int, false});  // not assignable by the generator
                line("while " + c + " < " + std::to_string(pick(0, 5)) + " {");
                ++indent_;
                line(c + " += 1;");
                --indent_;
                ++loop_depth_;
                block(depth - 1);
                --loop_depth_;
                line("}");
                break;
            }
            case 11: {  // for each
                auto arrs = vars_of(T::IntArr);
                std::string x = fresh("x");
                bool fresh_array = arrs.empty() || chance(30);
                std::string src = fresh_array ? expr(T::IntArr, 2)
                                              : arrs[static_cast<size_t>(pick(0, static_cast<int>(arrs.size()) - 1))]->name;
                line("for " + x + " in " + src + " {");
                ++loop_depth_;
                if (!fresh_array) iterating_.push_back(src);
                scopes_.emplace_back();
                scopes_.back().push_back(Var{x, T::Int, false});
                block_body(depth - 1);
                scopes_.pop_back();
                if (!fresh_array) iterating_.pop_back();
                --loop_depth_;
                line("}");
                break;
            }
            case 12: {  // closure capturing a variable
                std::string c = fresh("c");
                std::string z = fresh("z");
                scopes_.emplace_back();
                scopes_.back().push_back(Var{z, T::Int, false});
                std::string body = expr(T::Int, 2);
                scopes_.pop_back();
                line("let " + c + " = fn(" + z + ": Int) -> Int { return " + body + "; };");
                scopes_.back().push_back(Var{c, T::Fn, false});
                line("print(" + c + "(" + expr(T::Int, 1) + "));");
                break;
            }
            default: {  // match statement / break / continue
                if (loop_depth_ > 0 && chance(30)) {
                    std::string cond = expr(T::Bool, 2);
                    std::string what = chance(50) ? "break" : "continue";
                    line("if " + cond + " { " + what + "; }");
                    break;
                }
                line("match " + expr(T::E, 2) + " {");
                ++indent_;
                std::string a = fresh("m");
                scopes_.emplace_back();
                scopes_.back().push_back(Var{a, T::Int, false});
                line("E::A(" + a + ") => { print(" + expr(T::Int, 2) + "); }");
                scopes_.pop_back();
                line("E::B => { print(\"B\"); }");
                line("_ => { print(" + expr(T::Str, 2) + "); }");
                --indent_;
                line("}");
                break;
            }
        }
    }

    void block(int depth) {
        scopes_.emplace_back();
        block_body(depth);
        scopes_.pop_back();
    }

    void block_body(int depth) {
        ++indent_;
        int n = pick(1, 4);
        for (int i = 0; i < n; ++i) stmt(depth);
        --indent_;
    }

    void print_stmt() {
        T t = random_type();
        line("print(" + expr(t, 3) + ");");
    }

    void assign_stmt() {
        T t = random_type();
        auto vs = vars_of(t, true);
        if (vs.empty()) {
            print_stmt();
            return;
        }
        const Var* v = vs[static_cast<size_t>(pick(0, static_cast<int>(vs.size()) - 1))];
        if (t == T::Int && chance(50)) {
            const char* ops[] = {" += ", " -= ", " *= "};
            std::string op = ops[pick(0, 2)];
            std::string rhs = expr(T::Int, 2);
            line(v->name + op + rhs + ";");
        } else if (t == T::IntArr && chance(50)) {
            std::string idx = index_expr(v->name);
            std::string rhs = expr(T::Int, 2);
            line(v->name + "[" + idx + "] = " + rhs + ";");
        } else {
            line(v->name + " = " + expr(t, 3) + ";");
        }
    }

    T random_type() {
        int r = pick(0, 20);
        if (r < 8) return T::Int;
        if (r < 11) return T::Bool;
        if (r < 13) return T::Float;
        if (r < 16) return T::Str;
        if (r < 18) return T::IntArr;
        if (r < 19) return T::P;
        return T::E;
    }

    std::string index_expr(const std::string& arr) {
        // Usually in bounds; sometimes deliberately not.
        if (chance(85)) return "math.abs(" + expr(T::Int, 1) + ") % math.max(len(" + arr + "), 1)";
        return expr(T::Int, 1);
    }

    // ------------------------------------------------------------ expressions
    // Concatenate generated parts in a fixed order. C++ does not sequence the
    // operands of `+`, so `expr(..) + expr(..)` would make the output depend
    // on the compiler; every multi-part expression goes through these.
    std::string bin(const std::string& open, T a, int da, const std::string& mid, T b, int db, const std::string& close) {
        std::string x = expr(a, da);
        std::string y = expr(b, db);
        return open + x + mid + y + close;
    }

    std::string int_lit() {
        int r = pick(0, 20);
        if (r == 0) return "9223372036854775807";
        if (r == 1) return "(-9223372036854775807 - 1)";
        if (r < 4) return std::to_string(pick(-1000, 1000));
        return std::to_string(pick(0, 9));
    }

    std::string expr(T t, int depth) {
        if (depth <= 0) return leaf(t);
        switch (t) {
            case T::Int: {
                int r = pick(0, 14);
                switch (r) {
                    case 0:
                    case 1: return leaf(t);
                    case 2:
                    case 3:
                    case 4: {
                        const char* ops[] = {" + ", " - ", " * ", " / ", " % "};
                        int k = pick(0, 4);
                        std::string rhs = expr(T::Int, depth - 1);
                        if (k >= 3 && chance(80)) rhs = "(" + rhs + " % 5 + 6)";  // usually nonzero
                        return "(" + expr(T::Int, depth - 1) + ops[k] + rhs + ")";
                    }
                    case 5: return "-" + expr(T::Int, depth - 1);
                    case 6: return "len(" + expr(chance(50) ? T::Str : T::IntArr, depth - 1) + ")";
                    case 7: {
                        auto arrs = vars_of(T::IntArr);
                        if (arrs.empty()) return leaf(t);
                        const Var* a = arrs[static_cast<size_t>(pick(0, static_cast<int>(arrs.size()) - 1))];
                        return a->name + "[" + index_expr(a->name) + "]";
                    }
                    case 8: {
                        auto ps = vars_of(T::P);
                        if (ps.empty()) return "P { a: " + expr(T::Int, depth - 1) + ", b: 2 }.a";
                        std::string obj = ps[static_cast<size_t>(pick(0, static_cast<int>(ps.size()) - 1))]->name;
                        return obj + (chance(50) ? ".a" : ".b");
                    }
                    case 9: {
                        if (fns_.empty()) return leaf(t);
                        const FnSig f = fns_[static_cast<size_t>(pick(0, static_cast<int>(fns_.size()) - 1))];
                        std::string s = f.name + "(";
                        for (int i = 0; i < f.arity; ++i) {
                            if (i) s += ", ";
                            s += f.name == "rec" ? "math.abs(" + expr(T::Int, depth - 1) + ") % 20" : expr(T::Int, depth - 1);
                        }
                        return s + ")";
                    }
                    case 10: {
                        std::string a = fresh("q"), b = fresh("q");
                        std::string e = expr(T::E, depth - 1);
                        scopes_.emplace_back();
                        scopes_.back().push_back(Var{a, T::Int, false});
                        std::string arm1 = expr(T::Int, depth - 1);
                        scopes_.back().push_back(Var{b, T::Int, false});
                        std::string arm3 = expr(T::Int, depth - 1);
                        scopes_.pop_back();
                        return "match " + e + " { E::A(" + a + ") => " + arm1 + ", E::B => " + expr(T::Int, depth - 1) +
                               ", E::C(" + a + ", " + b + ") => " + arm3 + " }";
                    }
                    case 11: {
                        auto fs = vars_of(T::Fn);
                        if (fs.empty()) return leaf(t);
                        std::string callee = fs[static_cast<size_t>(pick(0, static_cast<int>(fs.size()) - 1))]->name;
                        std::string arg = expr(T::Int, depth - 1);
                        return callee + "(" + arg + ")";
                    }
                    case 12: return "int(" + expr(T::Float, depth - 1) + ")";
                    case 13: return bin("strings.find(", T::Str, depth - 1, ", ", T::Str, depth - 1, ")");
                    default: return bin("math.max(", T::Int, depth - 1, ", ", T::Int, depth - 1, ")");
                }
            }
            case T::Bool: {
                int r = pick(0, 7);
                switch (r) {
                    case 0: return leaf(t);
                    case 1:
                    case 2: {
                        const char* ops[] = {" < ", " <= ", " > ", " >= ", " == ", " != "};
                        std::string op = ops[pick(0, 5)];
                        return bin("(", T::Int, depth - 1, op, T::Int, depth - 1, ")");
                    }
                    case 3: {
                        std::string op = chance(50) ? " && " : " || ";
                        return bin("(", T::Bool, depth - 1, op, T::Bool, depth - 1, ")");
                    }
                    case 4: return "!" + expr(T::Bool, depth - 1);
                    case 5: {
                        std::string op = chance(50) ? " == " : " < ";
                        return bin("(", T::Str, depth - 1, op, T::Str, depth - 1, ")");
                    }
                    case 6: {
                        std::string op = chance(50) ? " < " : " == ";
                        return bin("(", T::Float, depth - 1, op, T::Float, depth - 1, ")");
                    }
                    default: return bin("(", T::Bool, depth - 1, " == ", T::Bool, depth - 1, ")");
                }
            }
            case T::Float: {
                int r = pick(0, 5);
                switch (r) {
                    case 0: return leaf(t);
                    case 1:
                    case 2: {
                        const char* ops[] = {" + ", " - ", " * ", " / "};
                        std::string op = ops[pick(0, 3)];
                        return bin("(", T::Float, depth - 1, op, T::Float, depth - 1, ")");
                    }
                    case 3: return "float(" + expr(T::Int, depth - 1) + ")";
                    case 4: return "math.sqrt(" + expr(T::Float, depth - 1) + ")";
                    default: return "-" + expr(T::Float, depth - 1);
                }
            }
            case T::Str: {
                int r = pick(0, 6);
                switch (r) {
                    case 0:
                    case 1: return leaf(t);
                    case 2: return bin("(", T::Str, depth - 1, " + ", T::Str, depth - 1, ")");
                    case 3: return "str(" + expr(random_type(), depth - 1) + ")";
                    case 4: return "strings.upper(" + expr(T::Str, depth - 1) + ")";
                    case 5: return "strings.join(strings.split(" + expr(T::Str, depth - 1) + ", \"a\"), \"-\")";
                    default: {
                        std::string x = expr(T::Str, depth - 1);
                        return "strings.repeat(" + x + ", " + std::to_string(pick(0, 3)) + ")";
                    }
                }
            }
            case T::IntArr: {
                int r = pick(0, 3);
                if (r == 0) return leaf(t);
                if (r == 1) {
                    std::string x = expr(T::Int, depth - 1);
                    return "[" + x + "; " + std::to_string(pick(0, 4)) + "]";
                }
                std::string s = "[";
                int n = pick(1, 4);  // [] alone has no inferable type
                for (int i = 0; i < n; ++i) s += (i ? ", " : "") + expr(T::Int, depth - 1);
                return s + "]";
            }
            case T::P: return bin("P { b: ", T::Int, depth - 1, ", a: ", T::Int, depth - 1, " }");
            case T::E: {
                int r = pick(0, 2);
                if (r == 0) return "E::A(" + expr(T::Int, depth - 1) + ")";
                if (r == 1) return "E::B";
                return bin("E::C(", T::Int, depth - 1, ", ", T::Int, depth - 1, ")");
            }
            case T::Fn: return leaf(t);
        }
        return leaf(t);
    }

    std::string leaf(T t) {
        auto vs = vars_of(t);
        if (!vs.empty() && chance(60)) return vs[static_cast<size_t>(pick(0, static_cast<int>(vs.size()) - 1))]->name;
        switch (t) {
            case T::Int: return int_lit();
            case T::Bool: return chance(50) ? "true" : "false";
            case T::Float: {
                const char* fs[] = {"0.5", "1.0", "2.25", "0.1", "3.0", "100.0", "0.0", "1.0e10"};
                return fs[pick(0, 7)];
            }
            case T::Str: {
                const char* ss[] = {"\"\"", "\"a\"", "\"hello\"", "\"banana\"", "\"x y\"", "\"A\\n\""};
                return ss[pick(0, 5)];
            }
            case T::IntArr: {
                std::string x = int_lit();
                std::string y = int_lit();
                return "[" + x + ", " + y + "]";
            }
            case T::P: return "P { a: 1, b: 2 }";
            case T::E: return "E::B";
            case T::Fn: return "fn(k: Int) -> Int { return k * 2; }";
        }
        return "0";
    }

    std::mt19937_64 rng_;
    std::ostringstream o_;
    std::vector<std::vector<Var>> scopes_;
    std::vector<FnSig> fns_;
    std::vector<std::string> iterating_;  // arrays iterated by enclosing for-each loops
    int indent_ = 0;
    int counter_ = 0;
    int loop_depth_ = 0;
    bool in_fn_ = false;
};

}  // namespace

std::string generate_program(uint64_t seed) { return Gen(seed).program(); }

}  // namespace ember
