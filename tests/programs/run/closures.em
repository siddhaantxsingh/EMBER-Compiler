fn make_counter() -> fn() -> Int {
    var count = 0;
    return fn() -> Int {
        count += 1;
        return count;
    };
}

fn compose(f: fn(Int) -> Int, g: fn(Int) -> Int) -> fn(Int) -> Int {
    return fn(x: Int) -> Int { return f(g(x)); };
}

fn main() {
    let a = make_counter();
    let b = make_counter();
    a();
    a();
    print(a());
    print(b());
    let inc = fn(x: Int) -> Int { return x + 1; };
    let dbl = fn(x: Int) -> Int { return x * 2; };
    print(compose(inc, dbl)(5));
    print(compose(dbl, inc)(5));
    var shared = 0;
    let set = fn(v: Int) { shared = v; };
    let get = fn() -> Int { return shared; };
    set(42);
    print(get());
    print(shared);
    print(inc);
}
