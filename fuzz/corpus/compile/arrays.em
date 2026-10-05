fn sum(xs: [Int]) -> Int {
    var total = 0;
    for x in xs {
        total += x;
    }
    return total;
}

fn main() {
    var xs = [3, 1, 4, 1, 5];
    print(xs);
    print(len(xs));
    print(xs[2]);
    xs[2] = 40;
    xs[0] += 10;
    push(xs, 9);
    print(xs);
    print(pop(xs));
    print(sum(xs));
    let grid = [[0; 3]; 2];
    grid[0][1] = 7;
    print(grid);
    let empty: [String] = [];
    print(empty);
    print(len(empty));
    let alias = xs;
    push(alias, 100);
    print(len(xs));
}
