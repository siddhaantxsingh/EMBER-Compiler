// Category: nested arrays and float arithmetic (matrix multiply).
fn make(n: Int, seed: Int) -> [[Float]] {
    var m: [[Float]] = [];
    var s = seed;
    for i in 0..n {
        var row: [Float] = [];
        for j in 0..n {
            s = (s * 1103515245 + 12345) % 2147483648;
            push(row, float(s % 1000) / 100.0);
        }
        push(m, row);
    }
    return m;
}

fn main() {
    let n = 200;
    let a = make(n, 1);
    let b = make(n, 2);
    var trace = 0.0;
    for i in 0..n {
        let ai = a[i];
        for j in 0..n {
            var sum = 0.0;
            for k in 0..n {
                sum += ai[k] * b[k][j];
            }
            if i == j { trace += sum; }
        }
    }
    print(int(trace));
}
