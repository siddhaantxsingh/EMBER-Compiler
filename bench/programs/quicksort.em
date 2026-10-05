// Category: recursion over arrays with swaps.
fn quicksort(xs: [Int], lo: Int, hi: Int) {
    if lo >= hi { return; }
    let pivot = xs[(lo + hi) / 2];
    var i = lo;
    var j = hi;
    while i <= j {
        while xs[i] < pivot { i += 1; }
        while xs[j] > pivot { j -= 1; }
        if i <= j {
            let t = xs[i];
            xs[i] = xs[j];
            xs[j] = t;
            i += 1;
            j -= 1;
        }
    }
    quicksort(xs, lo, j);
    quicksort(xs, i, hi);
}

fn main() {
    var seed = 42;
    var xs: [Int] = [];
    for k in 0..1000000 {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        push(xs, seed % 1000000);
    }
    quicksort(xs, 0, len(xs) - 1);
    var ok = true;
    for k in 1..len(xs) {
        if xs[k - 1] > xs[k] { ok = false; }
    }
    print(ok);
    print(xs[0] + xs[500000] + xs[999999]);
}
