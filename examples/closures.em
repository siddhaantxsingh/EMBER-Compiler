// Closures capture variables by reference and outlive their creator.
fn make_account(initial: Int) -> fn(Int) -> Int {
    var balance = initial;
    return fn(delta: Int) -> Int {
        balance += delta;
        return balance;
    };
}

fn main() {
    let account = make_account(100);
    account(50);
    account(-30);
    print(account(0));
    var handlers: [fn() -> String] = [];
    for i in 0..3 {
        handlers = push_handler(handlers, i);
    }
    for h in handlers {
        print(h());
    }
}

fn push_handler(hs: [fn() -> String], i: Int) -> [fn() -> String] {
    push(hs, fn() -> String { return "handler " + str(i); });
    return hs;
}
