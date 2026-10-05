// Category: array indexing and mutation.
fn main() {
    let n = 5000000;
    let composite = [false; n + 1];
    var count = 0;
    for i in 2..n + 1 {
        if !composite[i] {
            count += 1;
            var j = i * i;
            while j <= n {
                composite[j] = true;
                j += i;
            }
        }
    }
    print(count);
}
