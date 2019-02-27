#include <costmap_2d/costmap_layer.h>

namespace costmap_2d
{

void CostmapLayer::touch(const double x, const double y, double* min_x, double* min_y, double* max_x, double* max_y)
{
    *min_x = std::min(x, *min_x);
    *min_y = std::min(y, *min_y);
    *max_x = std::max(x, *max_x);
    *max_y = std::max(y, *max_y);
}

void CostmapLayer::matchSize()
{
    Costmap2D* master = layered_costmap_->getCostmap();
    resizeMap(master->getSizeInCellsX(), master->getSizeInCellsY(), master->getResolution(), master->getOriginX(),
              master->getOriginY());
}

// cppcheck-suppress unusedFunction
void CostmapLayer::addExtraBounds(const double mx0, const double my0, const double mx1, const double my1)
{
    extra_min_x_ = std::min(mx0, extra_min_x_);
    extra_max_x_ = std::max(mx1, extra_max_x_);
    extra_min_y_ = std::min(my0, extra_min_y_);
    extra_max_y_ = std::max(my1, extra_max_y_);
    has_extra_bounds_ = true;
}

void CostmapLayer::useExtraBounds(double* min_x, double* min_y, double* max_x, double* max_y)
{
    if (!has_extra_bounds_)
        return;

    *min_x = std::min(extra_min_x_, *min_x);
    *min_y = std::min(extra_min_y_, *min_y);
    *max_x = std::max(extra_max_x_, *max_x);
    *max_y = std::max(extra_max_y_, *max_y);
    extra_min_x_ = 1e6;
    extra_min_y_ = 1e6;
    extra_max_x_ = -1e6;
    extra_max_y_ = -1e6;
    has_extra_bounds_ = false;
}

void CostmapLayer::updateWithMax(costmap_2d::Costmap2D& master_grid, const unsigned int min_i, const unsigned int min_j,
                                 const unsigned int max_i, const unsigned int max_j)
{
    if (!enabled_)
        return;

    unsigned char* master_array = master_grid.getCharMap();
    const unsigned int span = master_grid.getSizeInCellsX();

    for (unsigned int j = min_j; j < max_j; j++)
    {
        unsigned int it = j * span + min_i;
        for (unsigned int i = min_i; i < max_i; i++)
        {
            if (costmap_[it] == NO_INFORMATION)
            {
                it++;
                continue;
            }

            unsigned char old_cost = master_array[it];
            if (old_cost == NO_INFORMATION || old_cost < costmap_[it])
                master_array[it] = costmap_[it];
            it++;
        }
    }
}

void CostmapLayer::updateWithTrueOverwrite(costmap_2d::Costmap2D& master_grid, const unsigned int min_i,
                                           const unsigned int min_j, const unsigned int max_i, const unsigned int max_j)
{
    if (!enabled_)
        return;

    unsigned char* master = master_grid.getCharMap();
    const unsigned int span = master_grid.getSizeInCellsX();

    for (unsigned int j = min_j; j < max_j; j++)
    {
        unsigned int it = span * j + min_i;
        for (unsigned int i = min_i; i < max_i; i++)
        {
            master[it] = costmap_[it];
            it++;
        }
    }
}

void CostmapLayer::updateWithOverwrite(costmap_2d::Costmap2D& master_grid, const unsigned int min_i,
                                       const unsigned int min_j, const unsigned int max_i, const unsigned int max_j)
{
    if (!enabled_)
        return;

    unsigned char* master = master_grid.getCharMap();
    const unsigned int span = master_grid.getSizeInCellsX();

    for (unsigned int j = min_j; j < max_j; j++)
    {
        unsigned int it = span * j + min_i;
        for (unsigned int i = min_i; i < max_i; i++)
        {
            if (costmap_[it] != NO_INFORMATION)
                master[it] = costmap_[it];
            it++;
        }
    }
}

// cppcheck-suppress unusedFunction
void CostmapLayer::updateWithAddition(costmap_2d::Costmap2D& master_grid, const unsigned int min_i,
                                      const unsigned int min_j, const unsigned int max_i, const unsigned int max_j)
{
    if (!enabled_)
        return;

    unsigned char* master_array = master_grid.getCharMap();
    const unsigned int span = master_grid.getSizeInCellsX();

    for (unsigned int j = min_j; j < max_j; j++)
    {
        unsigned int it = j * span + min_i;
        for (unsigned int i = min_i; i < max_i; i++)
        {
            if (costmap_[it] == NO_INFORMATION)
            {
                it++;
                continue;
            }

            const unsigned char old_cost = master_array[it];
            if (old_cost == NO_INFORMATION)
            {
                master_array[it] = costmap_[it];
            }
            else
            {
                const unsigned char sum =
                    std::max<unsigned char>(old_cost + costmap_[it], costmap_2d::INSCRIBED_INFLATED_OBSTACLE);
                master_array[it] = sum;
            }
            it++;
        }
    }
}
}  // namespace costmap_2d
