fn outer(a: Int) -> fn(Int) -> fn(Int) -> Int {
    return fn(b: Int) -> fn(Int) -> Int {
        return fn(c: Int) -> Int {
            return a * 100 + b * 10 + c;
        };
    };
}

fn main() {
    print(outer(1)(2)(3));
    let partial = outer(4)(5);
    print(partial(6));
    print(partial(7));
}
