// args: --max-heap=4M
struct Node { next: [Node], payload: [Int] }

fn main() {
    var total = 0;
    for i in 0..20000 {
        let a = Node { next: [], payload: [i; 20] };
        let b = Node { next: [a], payload: [] };
        push(a.next, b);
        total += len(a.payload);
    }
    print(total);
}
