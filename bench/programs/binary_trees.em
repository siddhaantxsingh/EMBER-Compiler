// Category: allocation and garbage collection (short-lived structs).
struct Node { left: [Node], right: [Node] }

fn make(depth: Int) -> Node {
    if depth == 0 { return Node { left: [], right: [] }; }
    return Node { left: [make(depth - 1)], right: [make(depth - 1)] };
}

fn check(n: Node) -> Int {
    if len(n.left) == 0 { return 1; }
    return 1 + check(n.left[0]) + check(n.right[0]);
}

fn main() {
    let long_lived = make(16);
    var total = 0;
    var d = 4;
    while d <= 16 {
        let iterations = 1;
        var i = 0;
        var iters = 1;
        var k = d;
        while k < 16 { iters *= 2; k += 2; }
        while i < iters {
            total += check(make(d));
            i += 1;
        }
        d += 2;
    }
    print(total);
    print(check(long_lived));
}
