#[derive(Clone, Debug)]
pub struct IdBitSet {
    bits: [u64; 1024],
}

impl IdBitSet {
    pub fn new() -> Self {
        Self { bits: [0; 1024] }
    }

    #[inline]
    pub fn insert(&mut self, id: u16) {
        let i = id as usize;
        self.bits[i >> 6] |= 1 << (i & 63);
    }

    #[inline]
    pub fn remove(&mut self, id: u16) {
        let i = id as usize;
        self.bits[i >> 6] &= !(1 << (i & 63));
    }

    #[inline]
    pub fn contains(&self, id: u16) -> bool {
        let i = id as usize;
        (self.bits[i >> 6] & (1 << (i & 63))) != 0
    }

    pub fn clear(&mut self) {
        self.bits.fill(0);
    }

    pub fn iter(&self) -> impl Iterator<Item = u16> + '_ {
        self.bits.iter().enumerate().flat_map(|(i, &word)| {
            let mut w = word;
            std::iter::from_fn(move || {
                if w == 0 {
                    None
                } else {
                    let tz = w.trailing_zeros();
                    w &= w - 1; // clear lowest set bit
                    Some(((i << 6) + tz as usize) as u16)
                }
            })
        })
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.bits.iter().map(|w| w.count_ones() as usize).sum()
    }
    
    pub fn is_empty(&self) -> bool {
        self.bits.iter().all(|&w| w == 0)
    }
}

impl Default for IdBitSet {
    fn default() -> Self {
        Self::new()
    }
}

impl PartialEq for IdBitSet {
    fn eq(&self, other: &Self) -> bool {
        self.bits == other.bits
    }
}
impl Eq for IdBitSet {}
