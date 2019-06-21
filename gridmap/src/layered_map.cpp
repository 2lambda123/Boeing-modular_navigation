#include <gridmap/layered_map.h>

namespace gridmap
{

LayeredMap::LayeredMap(const std::shared_ptr<BaseMapLayer> base_map_layer,
                       const std::vector<std::shared_ptr<Layer>>& layers)
    : base_map_layer_(base_map_layer), layers_(layers)
{
}


void LayeredMap::update()
{
    // copy from base map
    base_map_layer_->draw(map_data_->grid);

    // update from layers
    for (const auto& layer : layers_)
    {
        layer->update(map_data_->grid);
    }
}

void LayeredMap::update(const AABB& bb)
{
    // copy from base map
    base_map_layer_->draw(map_data_->grid, bb);

    // update from layers
    for (const auto& layer : layers_)
    {
        layer->update(map_data_->grid, bb);
    }
}

void LayeredMap::clearRadius(const Eigen::Vector2d& pose, const double radius)
{
    const Eigen::Vector2i cell_index = map_data_->grid.dimensions().getCellIndex(pose);
    const int cell_radius = radius / map_data_->grid.dimensions().resolution();

    // update from layers
    for (const auto& layer : layers_)
    {
        layer->clearRadius(cell_index, cell_radius);
    }
}

void LayeredMap::setMap(const hd_map::Map& hd_map, const nav_msgs::OccupancyGrid& map_data)
{
    base_map_layer_->setMap(hd_map, map_data);
    for (const auto& layer : layers_)
    {
        layer->setMap(hd_map, map_data);
    }
    map_data_ = std::make_shared<MapData>(hd_map, base_map_layer_->dimensions());
    update();
}
}
