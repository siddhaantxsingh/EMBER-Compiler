// args: --max-heap=8M
struct Pair { left: [Int], right: String }

fn churn(n: Int) -> Int {
    var keep = 0;
    for i in 0..n {
        let p = Pair { left: [i; 16], right: "item" + str(i) };
        keep += len(p.left) + len(p.right);
    }
    return keep;
}

fn main() {
    print(churn(200000));
}
