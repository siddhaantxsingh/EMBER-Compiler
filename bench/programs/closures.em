// Category: closures and indirect calls.
fn compose(f: fn(Int) -> Int, g: fn(Int) -> Int) -> fn(Int) -> Int {
    return fn(x: Int) -> Int { return f(g(x)); };
}

fn main() {
    var counter = 0;
    let bump = fn(x: Int) -> Int { counter += 1; return x + 1; };
    let double = fn(x: Int) -> Int { return x * 2 % 1000003; };
    let both = compose(bump, double);
    var acc = 1;
    for i in 0..3000000 {
        acc = both(acc);
    }
    print(acc);
    print(counter);
}
