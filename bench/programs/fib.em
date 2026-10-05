// Category: function calls and recursion.
fn fib(n: Int) -> Int {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn main() {
    print(fib(32));
}
