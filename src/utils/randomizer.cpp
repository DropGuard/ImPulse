#include "randomizer.h"

Randomizer::Randomizer() {
    setCount(0);
}

Randomizer::Randomizer(int _count) : m_currentIndex(0) {
    setCount(_count);
}

void Randomizer::setCount(int _count) {
    m_vec.resize(_count);
    fill();
    // a shrinking folder can strand the cursor past the new end (and a missed
    // setCurrent() can leave it at -1); nobody may index with that state
    if(m_currentIndex < 0 || m_currentIndex >= static_cast<int>(m_vec.size()))
        m_currentIndex = 0;
}

void Randomizer::shuffle() {
    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::shuffle(m_vec.begin(), m_vec.end(), rng);
}

void Randomizer::setCurrent(int _current) {
    m_currentIndex = indexOf(_current);
}

// this assumes our array contains shuffled int values [0 ... count]
// which it does in our case
int Randomizer::indexOf(int item) {
    auto it = std::find(m_vec.begin(), m_vec.end(), item);
    return (it == m_vec.end()) ? -1 : static_cast<int>(std::distance(m_vec.begin(), it));
}

void Randomizer::fill() {
    std::vector<int>::iterator it;
    int i = 0;
    for(it = m_vec.begin(); it != m_vec.end(); ++it, ++i) {
        *it = i;
    }
}

void Randomizer::print() {
    qDebug() << "---vector---";
    std::vector<int>::iterator it;
    for(it = m_vec.begin(); it != m_vec.end(); ++it) {
        std::cout << *it << std::endl;
    }
    qDebug() << "----end----";
}

int Randomizer::next() {
    if(m_vec.empty())
        return 0; // nothing to draw from; loadFileIndex no-ops on a bad index
    if(m_vec.size() == 1)
        return m_vec[0]; // reshuffling cannot move the cursor, and the loop below
                         // would spin on it forever, hanging the UI thread
    if(m_currentIndex < 0 || m_currentIndex >= static_cast<int>(m_vec.size()))
        m_currentIndex = 0;
    // re-shuffle when needed
    // because vector gets rearranged this will break prev()
    while(m_currentIndex == static_cast<int>(m_vec.size()) - 1) {
        int currentItem = m_vec[m_currentIndex];
        shuffle();
        setCurrent(currentItem);
    }
    m_currentIndex++;
    return m_vec[m_currentIndex];
}

int Randomizer::prev() {
    if(m_vec.empty())
        return 0;
    if(m_vec.size() == 1)
        return m_vec[0];
    if(m_currentIndex < 1 || m_currentIndex >= static_cast<int>(m_vec.size()))
        m_currentIndex = static_cast<int>(m_vec.size());
    while(m_currentIndex == 0) {
        int currentItem = m_vec[m_currentIndex];
        shuffle();
        setCurrent(currentItem);
    }
    m_currentIndex--;
    return m_vec[m_currentIndex];
}
