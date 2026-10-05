// Regression: match bindings in a global initializer (found by difftest).
enum E { A(Int), B }

var g: Int = match E::A(41) { E::A(v) => v + 1, E::B => 0 };

fn main() {
    print(g);
}
